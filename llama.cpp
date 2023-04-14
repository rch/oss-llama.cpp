// Defines fileno on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "llama_util.h"
#include "llama.h"

#include "ggml.h"

#include <array>
#include <cinttypes>
#include <fstream>
#include <random>
#include <map>
#include <unordered_map>
#include <queue>
#include <cassert>
#include <cstring>
#include <climits>
#include <memory>
#include <algorithm>
#include <initializer_list>
#include <sstream>


#define LLAMA_USE_SCRATCH
#define LLAMA_MAX_SCRATCH_BUFFERS 16


// available llama models
enum e_model {
    MODEL_UNKNOWN,
    MODEL_7B,
    MODEL_13B,
    MODEL_30B,
    MODEL_65B,
};

static const size_t MB = 1024*1024;

// computed for n_ctx == 2048
// TODO: dynamically determine these sizes
//       needs modifications in ggml

static const std::map<e_model, size_t> MEM_REQ_SCRATCH0 = {
    { MODEL_7B,    512ull*MB },
    { MODEL_13B,   512ull*MB },
    { MODEL_30B,   512ull*MB },
    { MODEL_65B,   512ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_SCRATCH1 = {
    { MODEL_7B,    512ull*MB },
    { MODEL_13B,   512ull*MB },
    { MODEL_30B,   512ull*MB },
    { MODEL_65B,   512ull*MB },
};

// 2*n_embd*n_ctx*n_layer*sizeof(float16)
static const std::map<e_model, size_t> MEM_REQ_KV_SELF = {
    { MODEL_7B,   1026ull*MB },
    { MODEL_13B,  1608ull*MB },
    { MODEL_30B,  3124ull*MB },
    { MODEL_65B,  5120ull*MB },
};

// this is mostly needed for temporary mul_mat buffers to dequantize the data
// not actually needed if BLAS is disabled
static const std::map<e_model, size_t> MEM_REQ_EVAL = {
    { MODEL_7B,   768ull*MB },
    { MODEL_13B, 1024ull*MB },
    { MODEL_30B, 1280ull*MB },
    { MODEL_65B, 1536ull*MB },
};

// default hparams (LLaMA 7B)
struct llama_hparams {
    uint32_t n_vocab = 32000;
    uint32_t n_ctx   = 512;   // this is provided as user input?
    uint32_t n_embd  = 4096;
    uint32_t n_mult  = 256;
    uint32_t n_head  = 32;
    uint32_t n_layer = 32;
    uint32_t n_rot   = 64;
    enum llama_ftype ftype = LLAMA_FTYPE_MOSTLY_F16;

    bool operator!=(const llama_hparams & other) const {
        return memcmp(this, &other, sizeof(llama_hparams));
    }
};

struct llama_layer {
    // normalization
    struct ggml_tensor * attention_norm;

    // attention
    struct ggml_tensor * wq;
    struct ggml_tensor * wk;
    struct ggml_tensor * wv;
    struct ggml_tensor * wo;

    // normalization
    struct ggml_tensor * ffn_norm;

    // ff
    struct ggml_tensor * w1;
    struct ggml_tensor * w2;
    struct ggml_tensor * w3;
};

struct llama_kv_cache {
    struct ggml_tensor * k;
    struct ggml_tensor * v;

    struct ggml_context * ctx = NULL;

    llama_buffer buf;

    int n; // number of tokens currently in the cache

    ~llama_kv_cache() {
        if (ctx) {
            ggml_free(ctx);
        }
    }
};

struct llama_model {
    e_model type = MODEL_UNKNOWN;

    llama_hparams hparams;

    struct ggml_tensor * tok_embeddings;

    struct ggml_tensor * norm;
    struct ggml_tensor * output;

    std::vector<llama_layer> layers;

    // context
    struct ggml_context * ctx = NULL;

    // key + value cache for the self attention
    // TODO: move to llama_state
    struct llama_kv_cache kv_self;

    // the model memory buffer
    llama_buffer buf;

    // model memory mapped file
    std::unique_ptr<llama_mmap> mapping;

    // objects representing data potentially being locked in memory
    llama_mlock mlock_buf;
    llama_mlock mlock_mmap;

    // for quantize-stats only
    std::vector<std::pair<std::string, struct ggml_tensor *>> tensors_by_name;

    ~llama_model() {
        if (ctx) {
            ggml_free(ctx);
        }
    }
};

struct llama_vocab {
    using id    = int32_t;
    using token = std::string;

    struct token_score {
        token tok;
        float score;
    };

    std::unordered_map<token, id> token_to_id;
    std::vector<token_score> id_to_token;
};

struct llama_context {
    std::mt19937 rng;

    int64_t t_load_us = 0;
    int64_t t_start_us = 0;
    bool has_evaluated_once = false;

    int64_t t_sample_us = 0;
    int64_t t_eval_us   = 0;
    int64_t t_p_eval_us = 0;

    int32_t n_sample = 0; // number of tokens sampled
    int32_t n_eval   = 0; // number of eval calls
    int32_t n_p_eval = 0; // number of tokens in eval calls for the prompt (with batch size > 1)

    llama_model model;
    llama_vocab vocab;

    size_t mem_per_token = 0;

    // decode output (2-dimensional array: [n_tokens][n_vocab])
    std::vector<float> logits;
    bool logits_all = false;

    // input embedding (1-dimensional array: [n_embd])
    std::vector<float> embedding;

    // memory buffers used to evaluate the model
    // TODO: move in llama_state
    llama_buffer buf_compute;
    llama_buffer buf_scratch[LLAMA_MAX_SCRATCH_BUFFERS];

    int    buf_last = 0;
    size_t buf_max_size[LLAMA_MAX_SCRATCH_BUFFERS] = { 0 };

    void use_buf(struct ggml_context * ctx, int i) {
#if defined(LLAMA_USE_SCRATCH)
        size_t last_size = 0;

        if (i == -1) {
            last_size = ggml_set_scratch(ctx, { 0, 0, nullptr, });
        } else {
            auto & buf = buf_scratch[i];
            last_size = ggml_set_scratch(ctx, { 0, buf.size, buf.addr, });
        }

        if (buf_last >= 0) {
            buf_max_size[buf_last] = std::max(buf_max_size[buf_last], last_size);
        }

        buf_last = i;
#else
        (void) i;
        (void) ctx;
#endif
    }

    size_t get_buf_max_mem(int i) const {
#if defined(LLAMA_USE_SCRATCH)
        return buf_max_size[i];
#else
        (void) i;
        return 0;
#endif
    }
};

template <typename T>
static T checked_mul(T a, T b) {
    T ret = a * b;
    if (a != 0 && ret / a != b) {
        throw format("overflow multiplying %llu * %llu",
                     (unsigned long long) a, (unsigned long long) b);
    }
    return ret;
}

static size_t checked_div(size_t a, size_t b) {
    if (b == 0 || a % b != 0) {
        throw format("error dividing %zu / %zu", a, b);
    }
    return a / b;
}

static std::string llama_format_tensor_shape(const std::vector<uint32_t> & ne) {
    std::string ret = "[" + std::to_string(ne.at(0));
    for (size_t i = 1; i < ne.size(); i++) {
        ret += " x " + std::to_string(ne.at(i));
    }
    ret += "]";
    return ret;
}

static const char * llama_format_type(enum ggml_type type) {
    switch (type) {
        case GGML_TYPE_F32: return "f32";
        case GGML_TYPE_F16: return "f16";
        case GGML_TYPE_Q4_0: return "q4_0";
        case GGML_TYPE_Q4_1: return "q4_1";
        default: LLAMA_ASSERT(false);
    }
}

static size_t llama_calc_tensor_size(const std::vector<uint32_t> & ne, enum ggml_type type) {
    size_t size = ggml_type_size(type);
    for (uint32_t dim : ne) {
        size = checked_mul<size_t>(size, dim);
    }
    return size / ggml_blck_size(type);
}

struct llama_load_tensor_shard {
    std::vector<uint32_t> ne;
    size_t size;
    enum ggml_type type;
    size_t file_idx;
    size_t file_off;

    void calc_size() {
        size = llama_calc_tensor_size(ne, type);
    }
};

enum llama_split_type {
    SPLIT_NONE,
    SPLIT_BY_COLUMNS,
    SPLIT_BY_ROWS
};

struct llama_load_tensor {
    std::vector<llama_load_tensor_shard> shards;

    std::string name;
    enum ggml_type type = GGML_TYPE_F32;
    llama_split_type split_type = SPLIT_NONE;
    std::vector<uint32_t> ne;
    size_t size;
    struct ggml_tensor * ggml_tensor = NULL;
    uint8_t * data;

    llama_load_tensor(const std::string & name) : name(name) {}

    void calc_all() {
        calc_type();
        calc_split_type();
        calc_ne();
        calc_size();
    }

    void calc_type() {
        const auto & first_shard = shards.at(0);
        for (const auto & shard : shards) {
            if (shard.type != first_shard.type) {
                throw format("inconsistent tensor shard type in '%s'", name.c_str());
            }
        }
        type = first_shard.type;
    }

    void calc_split_type() {
        if (shards.at(0).ne.size() == 1 || // 1D tensors are just duplicated in every file
            shards.size() == 1) { // only one file?
            split_type = SPLIT_NONE;
        } else if (name.find("tok_embeddings.") == 0 ||
            name.find(".attention.wo.weight") != std::string::npos ||
            name.find(".feed_forward.w2.weight") != std::string::npos) {
            split_type = SPLIT_BY_COLUMNS;
        } else {
            split_type = SPLIT_BY_ROWS;
        }
    }

    void calc_ne() {
        const auto & first_shard = shards.at(0);
        for (const auto & shard : shards) {
            if (shard.ne != first_shard.ne) {
                throw format("inconsistent tensor shard shape in '%s': first was %s, other was %s",
                             name.c_str(), llama_format_tensor_shape(first_shard.ne).c_str(), llama_format_tensor_shape(shard.ne).c_str());
            }
        }
        ne = first_shard.ne;
        LLAMA_ASSERT(shards.size() <= UINT32_MAX);
        uint32_t n_shards = (uint32_t) shards.size();
        switch (split_type) {
            case SPLIT_NONE:
                ne = first_shard.ne;
                break;
            case SPLIT_BY_COLUMNS:
                ne = {checked_mul<uint32_t>(first_shard.ne[0], n_shards),
                      first_shard.ne[1]};
                break;
            case SPLIT_BY_ROWS:
                ne = {first_shard.ne[0],
                      checked_mul<uint32_t>(first_shard.ne[1], n_shards)};
                break;
        }
    }

    void calc_size() {
        size = llama_calc_tensor_size(ne, type);
    }
};

struct llama_load_tensors_map {
    // tensors is kept in a separate vector to preserve file order
    std::vector<llama_load_tensor> tensors;
    std::unordered_map<std::string, size_t> name_to_idx;
};

enum llama_file_version {
    LLAMA_FILE_VERSION_GGML,
    LLAMA_FILE_VERSION_GGMF_V1, // added version field and scores in vocab
    LLAMA_FILE_VERSION_GGJT_V1, // added padding
};

struct llama_file_loader {
    llama_file file;
    llama_file_version file_version;
    llama_hparams hparams;
    llama_vocab vocab;

    llama_file_loader(const char * fname, size_t file_idx, llama_load_tensors_map & tensors_map)
        : file(fname, "rb") {
        fprintf(stderr, "llama.cpp: loading model from %s\n", fname);
        read_magic();
        read_hparams();
        read_vocab();
        read_tensor_metadata(file_idx, tensors_map);
    }
    void read_magic() {
        uint32_t magic = file.read_u32();
        uint32_t version = 0;

        if (magic != 'ggml') {
            version = file.read_u32();
        }

        if (magic == 'ggml' && version == 0) {
            file_version = LLAMA_FILE_VERSION_GGML;
        } else if (magic == 'ggmf' && version == 1) {
            file_version = LLAMA_FILE_VERSION_GGMF_V1;
        } else if (magic == 'ggjt' && version == 1) {
            file_version = LLAMA_FILE_VERSION_GGJT_V1;
        } else {
            throw format("unknown (magic, version) combination: %08x, %08x; is this really a GGML file?",
                         magic, version);
        }
    }
    void read_hparams() {
        hparams.n_vocab = file.read_u32();
        hparams.n_embd = file.read_u32();
        hparams.n_mult = file.read_u32();
        hparams.n_head = file.read_u32();
        hparams.n_layer = file.read_u32();
        hparams.n_rot = file.read_u32();
        hparams.ftype = (enum llama_ftype) file.read_u32();
    }
    void read_vocab() {
        vocab.id_to_token.resize(hparams.n_vocab);

        for (uint32_t i = 0; i < hparams.n_vocab; i++) {
            uint32_t len = file.read_u32();
            std::string word = file.read_string(len);

            float score = 0.0f;
            if (file_version >= LLAMA_FILE_VERSION_GGMF_V1) {
                file.read_raw(&score, sizeof(score));
            }

            vocab.token_to_id[word] = i;

            auto & tok_score = vocab.id_to_token[i];
            tok_score.tok = std::move(word);
            tok_score.score = score;
        }
    }
    void read_tensor_metadata(size_t file_idx, llama_load_tensors_map & tensors_map) {
        while (file.tell() < file.size) {
            llama_load_tensor_shard shard;
            uint32_t n_dims = file.read_u32();
            uint32_t name_len = file.read_u32();
            shard.type = (enum ggml_type) file.read_u32();
            shard.ne.resize(n_dims);
            file.read_raw(shard.ne.data(), sizeof(shard.ne[0]) * n_dims);
            std::string name = file.read_string(name_len);
            if (n_dims < 1 || n_dims > 2) {
                throw format("llama.cpp: tensor '%s' should not be %u-dimensional", name.c_str(), n_dims);
            }
            switch (shard.type) {
                case GGML_TYPE_F32:
                case GGML_TYPE_F16:
                case GGML_TYPE_Q4_0:
                case GGML_TYPE_Q4_1:
                    break;
                default: {
                    throw format("unrecognized tensor type %u\n", shard.type);
                }
            }

            if (file_version >= LLAMA_FILE_VERSION_GGJT_V1) {
                // skip to the next multiple of 32 bytes
                file.seek(-file.tell() & 31, SEEK_CUR);
            }
            shard.file_idx = file_idx;
            shard.file_off = file.tell();

            shard.calc_size();
            file.seek(shard.size, SEEK_CUR);

            auto it = tensors_map.name_to_idx.find(name);
            size_t idx;
            if (it != tensors_map.name_to_idx.end()) {
                idx = it->second;
            } else {
                tensors_map.tensors.emplace_back(name);
                idx = tensors_map.tensors.size() - 1;
                tensors_map.name_to_idx.emplace(name, idx);
            }
            tensors_map.tensors.at(idx).shards.push_back(shard);
        }
    }
};

struct llama_file_saver {
    llama_file file;
    llama_file_loader * any_file_loader;
    llama_file_saver(const char * fname, llama_file_loader * any_file_loader, enum llama_ftype new_ftype)
        : file(fname, "wb"), any_file_loader(any_file_loader) {
        fprintf(stderr, "llama.cpp: saving model to %s\n", fname);
        write_magic();
        write_hparams(new_ftype);
        write_vocab();
    }
    void write_magic() {
        file.write_u32('ggjt'); // magic
        file.write_u32(1); // version
    }
    void write_hparams(enum llama_ftype new_ftype) {
        const llama_hparams & hparams = any_file_loader->hparams;
        file.write_u32(hparams.n_vocab);
        file.write_u32(hparams.n_embd);
        file.write_u32(hparams.n_mult);
        file.write_u32(hparams.n_head);
        file.write_u32(hparams.n_layer);
        file.write_u32(hparams.n_rot);
        file.write_u32(new_ftype);
    }
    void write_vocab() {
        if (any_file_loader->file_version == LLAMA_FILE_VERSION_GGML) {
            fprintf(stderr, "llama.cpp: WARNING: input is an old file that doesn't have scores; will add dummy scores\n");
        }
        uint32_t n_vocab = any_file_loader->hparams.n_vocab;
        for (uint32_t i = 0; i < n_vocab; i++) {
            const auto & token_score = any_file_loader->vocab.id_to_token.at(i);
            file.write_u32((uint32_t) token_score.tok.size());
            file.write_raw(token_score.tok.data(), token_score.tok.size());
            file.write_raw(&token_score.score, sizeof(token_score.score));
        }
    }
    void write_tensor(llama_load_tensor & tensor, enum ggml_type new_type, const void * new_data, size_t new_size) {
        switch (new_type) {
            case GGML_TYPE_F32:
            case GGML_TYPE_F16:
            case GGML_TYPE_Q4_0:
            case GGML_TYPE_Q4_1:
                break;
            default: LLAMA_ASSERT(false);
        }
        file.write_u32((uint32_t) tensor.ne.size());
        file.write_u32((uint32_t) tensor.name.size());
        file.write_u32(new_type);
        file.write_raw(tensor.ne.data(), sizeof(tensor.ne[0]) * tensor.ne.size());
        file.write_raw(tensor.name.data(), tensor.name.size());
        file.seek(-file.tell() & 31, SEEK_CUR);
        LLAMA_ASSERT(new_size == llama_calc_tensor_size(tensor.ne, new_type));
        file.write_raw(new_data, new_size);
    }
};

struct llama_model_loader {
    std::vector<std::unique_ptr<llama_file_loader>> file_loaders;
    llama_load_tensors_map tensors_map;
    bool use_mmap;
    size_t num_ggml_tensors_created = 0;
    struct ggml_context * ggml_ctx = NULL;
    std::unique_ptr<llama_mmap> mapping;

    llama_model_loader(const std::string & fname_base, bool use_mmap, bool vocab_only) {
        auto first_file = new llama_file_loader(fname_base.c_str(), 0, tensors_map);
        file_loaders.emplace_back(first_file);
        uint32_t n_parts = vocab_only ? 1 : guess_n_parts();
        for (uint32_t i = 1; i < n_parts; i++) {
            std::string fname = fname_base + "." + std::to_string(i);
            auto ith_file = new llama_file_loader(fname.c_str(), i, tensors_map);
            file_loaders.emplace_back(ith_file);
            if (ith_file->hparams != first_file->hparams) {
                throw format("llama.cpp: hparams inconsistent between files");
            }
        }
        if (!llama_mmap::SUPPORTED) {
            use_mmap = false;
        }
        if (use_mmap && alignment_prevents_mmap()) {
            fprintf(stderr, "llama.cpp: can't use mmap because tensors are not aligned; convert to new format to avoid this\n");
            use_mmap = false;
        }
        this->use_mmap = use_mmap;
        for (llama_load_tensor & lt : tensors_map.tensors) {
            lt.calc_all();
        }
    }

    bool alignment_prevents_mmap() {
        for (const llama_load_tensor & lt : tensors_map.tensors) {
            for (const llama_load_tensor_shard & shard : lt.shards) {
                if (shard.file_off & 3) {
                    return true;
                }
            }
        }
        return false;
    }

    uint32_t guess_n_parts() const {
        auto it = tensors_map.name_to_idx.find("tok_embeddings.weight");
        if (it == tensors_map.name_to_idx.end()) {
            throw std::string("missing tok_embeddings.weight");
        }
        const llama_load_tensor & lt = tensors_map.tensors.at(it->second);
        return file_loaders.at(0)->hparams.n_embd / lt.shards.at(0).ne.at(0);
    }

    void calc_sizes(size_t * ctx_size_p, size_t * mmapped_size_p) const {
        *ctx_size_p = *mmapped_size_p = 0;
        for (const llama_load_tensor & lt : tensors_map.tensors) {
            *ctx_size_p += sizeof(struct ggml_tensor) + GGML_OBJECT_SIZE;
            *(use_mmap ? mmapped_size_p : ctx_size_p) += lt.size;
        }
    }

    struct ggml_tensor * get_tensor(const std::string & name, std::vector<uint32_t> ne) {
        auto it = tensors_map.name_to_idx.find(name);
        if (it == tensors_map.name_to_idx.end()) {
            throw format("llama.cpp: tensor '%s' is missing from model", name.c_str());
        }
        llama_load_tensor & lt = tensors_map.tensors.at(it->second);
        if (lt.ne != ne) {
            throw format("llama.cpp: tensor '%s' has wrong shape; expected %s, got %s",
                         name.c_str(), llama_format_tensor_shape(ne).c_str(), llama_format_tensor_shape(lt.ne).c_str());
        }
        return get_tensor_for(lt);
    }

    struct ggml_tensor * get_tensor_for(llama_load_tensor & lt) {
        struct ggml_tensor * tensor;
        if (lt.ne.size() == 2) {
            tensor = ggml_new_tensor_2d(ggml_ctx, lt.type, lt.ne.at(0), lt.ne.at(1));
        } else {
            LLAMA_ASSERT(lt.ne.size() == 1);
            tensor = ggml_new_tensor_1d(ggml_ctx, lt.type, lt.ne.at(0));
        }
        LLAMA_ASSERT(lt.ggml_tensor == NULL); // if this fails, we called get_tensor twice on the same tensor
        lt.ggml_tensor = tensor;
        num_ggml_tensors_created++;
        return tensor;
    }

    void done_getting_tensors() {
        if (num_ggml_tensors_created != tensors_map.tensors.size()) {
            throw std::string("llama.cpp: file contained more tensors than expected");
        }
    }

    void load_all_data(llama_progress_callback progress_callback, void *  progress_callback_user_data, llama_mlock * lmlock) {
        size_t data_size = 0;
        for (const llama_load_tensor & lt : tensors_map.tensors) {
            data_size += lt.size;
        }

        if (use_mmap) {
            mapping.reset(new llama_mmap(&file_loaders.at(0)->file));
            if (!lmlock) {
                // Don't call the callback since the actual loading will be lazy
                // and we can't measure it.
                progress_callback = NULL;
            }
            if (lmlock) {
                lmlock->init(mapping->addr);
            }
        }

        size_t done_size = 0;
        for (llama_load_tensor & lt : tensors_map.tensors) {
            if (progress_callback) {
                progress_callback((float) done_size / data_size, progress_callback_user_data);
            }
            LLAMA_ASSERT(lt.ggml_tensor); // unused tensors should have been caught by load_data already
            lt.data = (uint8_t *) lt.ggml_tensor->data;
            load_data_for(lt);
            lt.ggml_tensor->data = lt.data;
            done_size += lt.size;
            if (use_mmap && lmlock) {
                lmlock->grow_to(done_size);
            }
        }
        if (progress_callback) {
            progress_callback(1.0f, progress_callback_user_data);
        }
    }

    void load_data_for(llama_load_tensor & lt) {
        if (use_mmap) {
            LLAMA_ASSERT(lt.shards.size() == 1);
            lt.data = (uint8_t *) mapping->addr + lt.shards.at(0).file_off;
        } else if (lt.split_type == SPLIT_NONE) {
            llama_file & file = file_loaders.at(lt.shards.at(0).file_idx)->file;
            file.seek(lt.shards.at(0).file_off, SEEK_SET);
            file.read_raw(lt.data, lt.size);
        } else if (lt.split_type == SPLIT_BY_ROWS) {
            size_t offset = 0;
            for (llama_load_tensor_shard & shard : lt.shards) {
                llama_file & file = file_loaders.at(shard.file_idx)->file;
                file.seek(shard.file_off, SEEK_SET);
                file.read_raw(lt.data + offset, shard.size);
                offset += shard.size;
            }
            LLAMA_ASSERT(offset == lt.size);
        } else if (lt.split_type == SPLIT_BY_COLUMNS) {
            // Let's load the data into temporary buffers to ensure the OS performs large loads.
            std::vector<llama_buffer> tmp_bufs;
            tmp_bufs.resize(lt.shards.size());
            for (size_t i = 0; i < lt.shards.size(); i++) {
                llama_load_tensor_shard & shard = lt.shards.at(i);
                llama_file & file = file_loaders.at(shard.file_idx)->file;
                file.seek(shard.file_off, SEEK_SET);
                tmp_bufs.at(i).resize(shard.size);
                file.read_raw(tmp_bufs.at(i).addr, shard.size);
            }
            // Then reshape.
            size_t num_rows = lt.ne.at(1);
            size_t per_shard_row_size = lt.shards.at(0).size / num_rows;
            size_t out_offset = 0;
            for (size_t row = 0; row < num_rows; row++) {
                for (llama_buffer & tmp_buf : tmp_bufs) {
                    memcpy(lt.data + out_offset,
                           tmp_buf.addr + row * per_shard_row_size,
                           per_shard_row_size);
                    out_offset += per_shard_row_size;
                }
            }
            LLAMA_ASSERT(out_offset == lt.size);
        }
        if (0) {
            print_checksum(lt);
        }
    }

    static void print_checksum(llama_load_tensor & lt) {
        uint32_t sum = 0;
        for (size_t i = 0; i < lt.size; i++) {
            uint8_t byte = lt.data[i];
            sum = byte + (sum << 6) + (sum << 16) - sum; // sdbm hash
        }
        fprintf(stderr, "%s checksum: %#08x (%s, size %zu)\n", lt.name.c_str(), sum,
                llama_format_tensor_shape(lt.ne).c_str(), lt.size);
    }

};


//
// kv cache
//

static bool kv_cache_init(
        const struct llama_hparams & hparams,
             struct llama_kv_cache & cache,
                         ggml_type   wtype,
                               int   n_ctx) {
    const int n_embd  = hparams.n_embd;
    const int n_layer = hparams.n_layer;

    const int64_t n_mem      = (int64_t)n_layer*n_ctx;
    const int64_t n_elements = n_embd*n_mem;

    cache.buf.resize(2u*n_elements*ggml_type_size(wtype) + 2u*MB);

    struct ggml_init_params params;
    params.mem_size   = cache.buf.size;
    params.mem_buffer = cache.buf.addr;
    params.no_alloc   = false;

    cache.ctx = ggml_init(params);

    if (!cache.ctx) {
        fprintf(stderr, "%s: failed to allocate memory for kv cache\n", __func__);
        return false;
    }

    cache.k = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);
    cache.v = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);

    return true;
}

struct llama_context_params llama_context_default_params() {
    struct llama_context_params result = {
        /*.n_ctx                       =*/ 512,
        /*.n_parts                     =*/ -1,
        /*.seed                        =*/ 0,
        /*.f16_kv                      =*/ false,
        /*.logits_all                  =*/ false,
        /*.vocab_only                  =*/ false,
        /*.use_mmap                    =*/ true,
        /*.use_mlock                   =*/ false,
        /*.embedding                   =*/ false,
        /*.progress_callback           =*/ nullptr,
        /*.progress_callback_user_data =*/ nullptr,
    };

    return result;
}

bool llama_mmap_supported() {
    return llama_mmap::SUPPORTED;
}

bool llama_mlock_supported() {
    return llama_mlock::SUPPORTED;
}

//
// model loading
//

static const char *llama_file_version_name(llama_file_version version) {
    switch (version) {
        case LLAMA_FILE_VERSION_GGML: return "'ggml' (old version with low tokenizer quality and no mmap support)";
        case LLAMA_FILE_VERSION_GGMF_V1: return "ggmf v1 (old version with no mmap support)";
        case LLAMA_FILE_VERSION_GGJT_V1: return "ggjt v1 (latest)";
        default: LLAMA_ASSERT(false);
    }
}

static const char *llama_ftype_name(enum llama_ftype ftype) {
    switch (ftype) {
        case LLAMA_FTYPE_ALL_F32:     return "all F32";
        case LLAMA_FTYPE_MOSTLY_F16:  return "mostly F16";
        case LLAMA_FTYPE_MOSTLY_Q4_0: return "mostly Q4_0";
        case LLAMA_FTYPE_MOSTLY_Q4_1: return "mostly Q4_1";
        case LLAMA_FTYPE_MOSTLY_Q4_1_SOME_F16:
                                      return "mostly Q4_1, some F16";
        default:                      return "unknown, may not work";
    }
}

static const char *llama_model_type_name(e_model type) {
    switch (type) {
        case MODEL_7B: return "7B";
        case MODEL_13B: return "13B";
        case MODEL_30B: return "30B";
        case MODEL_65B: return "65B";
        default: LLAMA_ASSERT(false);
    }
}

static void llama_model_load_internal(
        const std::string & fname,
        llama_context & lctx,
        int n_ctx,
        ggml_type memory_type,
        bool use_mmap,
        bool use_mlock,
        bool vocab_only,
        llama_progress_callback progress_callback,
        void * progress_callback_user_data) {

    lctx.t_start_us = ggml_time_us();

    std::unique_ptr<llama_model_loader> ml(new llama_model_loader(fname, use_mmap, vocab_only));

    lctx.vocab = std::move(ml->file_loaders.at(0)->vocab);
    auto & model = lctx.model;
    model.hparams = ml->file_loaders.at(0)->hparams;
    llama_file_version file_version = ml->file_loaders.at(0)->file_version;
    auto & hparams = model.hparams;
    uint32_t n_ff = ((2*(4*hparams.n_embd)/3 + hparams.n_mult - 1)/hparams.n_mult)*hparams.n_mult;

    {
        switch (hparams.n_layer) {
            case 32: model.type = e_model::MODEL_7B; break;
            case 40: model.type = e_model::MODEL_13B; break;
            case 60: model.type = e_model::MODEL_30B; break;
            case 80: model.type = e_model::MODEL_65B; break;
        }

        hparams.n_ctx = n_ctx;
    }

    {
        fprintf(stderr, "%s: format     = %s\n",  __func__, llama_file_version_name(file_version));
        fprintf(stderr, "%s: n_vocab    = %u\n",  __func__, hparams.n_vocab);
        fprintf(stderr, "%s: n_ctx      = %u\n",  __func__, hparams.n_ctx);
        fprintf(stderr, "%s: n_embd     = %u\n",  __func__, hparams.n_embd);
        fprintf(stderr, "%s: n_mult     = %u\n",  __func__, hparams.n_mult);
        fprintf(stderr, "%s: n_head     = %u\n",  __func__, hparams.n_head);
        fprintf(stderr, "%s: n_layer    = %u\n",  __func__, hparams.n_layer);
        fprintf(stderr, "%s: n_rot      = %u\n",  __func__, hparams.n_rot);
        fprintf(stderr, "%s: ftype      = %u (%s)\n", __func__, hparams.ftype, llama_ftype_name(hparams.ftype));
        fprintf(stderr, "%s: n_ff       = %u\n",  __func__, n_ff);
        fprintf(stderr, "%s: n_parts    = %zu\n", __func__, ml->file_loaders.size());
        fprintf(stderr, "%s: model size = %s\n",  __func__, llama_model_type_name(model.type));
    }

    if (vocab_only) {
        return;
    }

    auto & ctx = model.ctx;

    size_t ctx_size, mmapped_size;
    ml->calc_sizes(&ctx_size, &mmapped_size);
    fprintf(stderr, "%s: ggml ctx size = %6.2f KB\n", __func__, ctx_size/1024.0);

    // print memory requirements
    {
        const size_t scale = memory_type == GGML_TYPE_F32 ? 2 : 1;

        // this is the total memory required to run the inference
        const size_t mem_required =
            ctx_size +
            mmapped_size +
            MEM_REQ_SCRATCH0.at(model.type) +
            MEM_REQ_SCRATCH1.at(model.type) +
            MEM_REQ_EVAL.at    (model.type);

        // this is the memory required by one llama_state
        const size_t mem_required_state =
            scale*MEM_REQ_KV_SELF.at(model.type);

        fprintf(stderr, "%s: mem required  = %7.2f MB (+ %7.2f MB per state)\n", __func__,
                mem_required / 1024.0 / 1024.0, mem_required_state / 1024.0 / 1024.0);
    }

    // create the ggml context
    {
        lctx.model.buf.resize(ctx_size);
        if (use_mlock) {
            lctx.model.mlock_buf.init(lctx.model.buf.addr);
            lctx.model.mlock_buf.grow_to(lctx.model.buf.size);
        }

        struct ggml_init_params params = {
            /*.mem_size   =*/ lctx.model.buf.size,
            /*.mem_buffer =*/ lctx.model.buf.addr,
            /*.no_alloc   =*/ ml->use_mmap,
        };

        model.ctx = ggml_init(params);
        if (!model.ctx) {
            throw format("ggml_init() failed");
        }
    }

    // prepare memory for the weights
    {
        const auto & hparams = model.hparams;

        const uint32_t n_embd  = hparams.n_embd;
        const uint32_t n_layer = hparams.n_layer;
        const uint32_t n_vocab = hparams.n_vocab;

        ml->ggml_ctx = ctx;

        model.tok_embeddings = ml->get_tensor("tok_embeddings.weight", {n_embd, n_vocab});
        model.norm   = ml->get_tensor("norm.weight", {n_embd});
        model.output = ml->get_tensor("output.weight", {n_embd, n_vocab});

        model.layers.resize(n_layer);
        for (uint32_t i = 0; i < n_layer; ++i) {
            auto & layer = model.layers[i];

            std::string layers_i = "layers." + std::to_string(i);

            layer.attention_norm = ml->get_tensor(layers_i + ".attention_norm.weight", {n_embd});

            layer.wq = ml->get_tensor(layers_i + ".attention.wq.weight", {n_embd, n_embd});
            layer.wk = ml->get_tensor(layers_i + ".attention.wk.weight", {n_embd, n_embd});
            layer.wv = ml->get_tensor(layers_i + ".attention.wv.weight", {n_embd, n_embd});
            layer.wo = ml->get_tensor(layers_i + ".attention.wo.weight", {n_embd, n_embd});

            layer.ffn_norm = ml->get_tensor(layers_i + ".ffn_norm.weight", {n_embd});

            layer.w1 = ml->get_tensor(layers_i + ".feed_forward.w1.weight", {n_embd,   n_ff});
            layer.w2 = ml->get_tensor(layers_i + ".feed_forward.w2.weight", {  n_ff,   n_embd});
            layer.w3 = ml->get_tensor(layers_i + ".feed_forward.w3.weight", {n_embd,   n_ff});
        }
    }

    ml->done_getting_tensors();

    // populate `tensors_by_name`
    for (llama_load_tensor & lt : ml->tensors_map.tensors) {
        model.tensors_by_name.emplace_back(lt.name, lt.ggml_tensor);
    }

    ml->load_all_data(progress_callback, progress_callback_user_data, use_mlock ? &lctx.model.mlock_mmap : NULL);

    model.mapping = std::move(ml->mapping);

    // loading time will be recalculate after the first eval, so
    // we take page faults deferred by mmap() into consideration
    lctx.t_load_us = ggml_time_us() - lctx.t_start_us;
}

static bool llama_model_load(
        const std::string & fname,
        llama_context & lctx,
        int n_ctx,
        ggml_type memory_type,
        bool use_mmap,
        bool use_mlock,
        bool vocab_only,
        llama_progress_callback progress_callback,
        void *progress_callback_user_data) {
    try {
        llama_model_load_internal(fname, lctx, n_ctx, memory_type, use_mmap, use_mlock,
                                  vocab_only, progress_callback, progress_callback_user_data);
        return true;
    } catch (const std::string & err) {
        fprintf(stderr, "error loading model: %s\n", err.c_str());
        return false;
    }
}

// evaluate the transformer
//
//   - lctx:      llama context
//   - tokens:    new batch of tokens to process
//   - n_past:    the context size so far
//   - n_threads: number of threads to use
//
static bool llama_eval_internal(
        llama_context & lctx,
    const llama_token * tokens,
            const int   n_tokens,
            const int   n_past,
            const int   n_threads) {
    const int64_t t_start_us = ggml_time_us();

    const int N = n_tokens;

    const auto & model   = lctx.model;
    const auto & hparams = model.hparams;

    auto & kv_self = model.kv_self;

    LLAMA_ASSERT(!!kv_self.ctx);

    const int n_embd  = hparams.n_embd;
    const int n_layer = hparams.n_layer;
    const int n_ctx   = hparams.n_ctx;
    const int n_head  = hparams.n_head;
    const int n_vocab = hparams.n_vocab;
    const int n_rot   = hparams.n_embd/hparams.n_head;

    auto & mem_per_token = lctx.mem_per_token;
    auto & buf_compute   = lctx.buf_compute;

    struct ggml_init_params params = {
        /*.mem_size   =*/ buf_compute.size,
        /*.mem_buffer =*/ buf_compute.addr,
        /*.no_alloc   =*/ false,
    };

    struct ggml_context * ctx0 = ggml_init(params);

    // for big prompts, if BLAS is enabled, it is better to use only one thread
    // otherwise, the threads are spin-lock waiting for the BLAS calls and are degrading the performance
    ggml_cgraph gf = {};
    gf.n_threads = N >= 32 && ggml_cpu_has_blas() ? 1 : n_threads;

    struct ggml_tensor * embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
    memcpy(embd->data, tokens, N*ggml_element_size(embd));

    struct ggml_tensor * inpL = ggml_get_rows(ctx0, model.tok_embeddings, embd);

    for (int il = 0; il < n_layer; ++il) {
        struct ggml_tensor * inpSA = inpL;

        struct ggml_tensor * cur;

        lctx.use_buf(ctx0, 0);

        // norm
        {
            cur = ggml_rms_norm(ctx0, inpL);

            // cur = attention_norm*cur
            cur = ggml_mul(ctx0,
                        ggml_repeat(ctx0, model.layers[il].attention_norm, cur),
                        cur);
        }

        // self-attention
        {
            // compute Q and K and RoPE them
            struct ggml_tensor * Qcur = ggml_rope(ctx0, ggml_reshape_3d(ctx0, ggml_mul_mat(ctx0, model.layers[il].wq, cur), n_embd/n_head, n_head, N), n_past, n_rot, 0);
            struct ggml_tensor * Kcur = ggml_rope(ctx0, ggml_reshape_3d(ctx0, ggml_mul_mat(ctx0, model.layers[il].wk, cur), n_embd/n_head, n_head, N), n_past, n_rot, 0);

            // store key and value to memory
            {
                // compute the transposed [N, n_embd] V matrix
                struct ggml_tensor * Vcur = ggml_transpose(ctx0, ggml_reshape_2d(ctx0, ggml_mul_mat(ctx0, model.layers[il].wv, cur), n_embd, N));

                struct ggml_tensor * k = ggml_view_1d(ctx0, kv_self.k, N*n_embd, (ggml_element_size(kv_self.k)*n_embd)*(il*n_ctx + n_past));
                struct ggml_tensor * v = ggml_view_2d(ctx0, kv_self.v, N, n_embd,
                        (   n_ctx)*ggml_element_size(kv_self.v),
                        (il*n_ctx)*ggml_element_size(kv_self.v)*n_embd + n_past*ggml_element_size(kv_self.v));

                // important: storing RoPE-ed version of K in the KV cache!
                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
            }

            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        Qcur,
                        0, 2, 1, 3);

            struct ggml_tensor * K =
                ggml_permute(ctx0,
                        ggml_reshape_3d(ctx0,
                            ggml_view_1d(ctx0, kv_self.k, (n_past + N)*n_embd, il*n_ctx*ggml_element_size(kv_self.k)*n_embd),
                            n_embd/n_head, n_head, n_past + N),
                        0, 2, 1, 3);

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q);

            // KQ_scaled = KQ / sqrt(n_embd/n_head)
            struct ggml_tensor * KQ_scaled =
                ggml_scale(ctx0,
                        KQ,
                        ggml_new_f32(ctx0, 1.0f/sqrtf(float(n_embd)/n_head)));

            // KQ_masked = mask_past(KQ_scaled)
            struct ggml_tensor * KQ_masked = ggml_diag_mask_inf(ctx0, KQ_scaled, n_past);

            // KQ = soft_max(KQ_masked)
            struct ggml_tensor * KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

            // split cached V into n_head heads
            struct ggml_tensor * V =
                ggml_view_3d(ctx0, kv_self.v,
                        n_past + N, n_embd/n_head, n_head,
                        n_ctx*ggml_element_size(kv_self.v),
                        n_ctx*ggml_element_size(kv_self.v)*n_embd/n_head,
                        il*n_ctx*ggml_element_size(kv_self.v)*n_embd);

#if 1
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V, KQ_soft_max);
#else
            // make V contiguous in memory to speed up the matmul, however we waste time on the copy
            // on M1 this is faster for the perplexity computation, but ~5% slower for the single-token generation
            // is there a better way?
            struct ggml_tensor * V_cont = ggml_cpy(ctx0, V, ggml_new_tensor_3d(ctx0, kv_self.v->type, n_past + N, n_embd/n_head, n_head));
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_cont, KQ_soft_max);
#endif

            // KQV_merged = KQV.permute(0, 2, 1, 3)
            struct ggml_tensor * KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

            // cur = KQV_merged.contiguous().view(n_embd, N)
            cur = ggml_cpy(ctx0,
                    KQV_merged,
                    ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));

            // projection (no bias)
            cur = ggml_mul_mat(ctx0,
                    model.layers[il].wo,
                    cur);
        }

        lctx.use_buf(ctx0, 1);

        struct ggml_tensor * inpFF = ggml_add(ctx0, cur, inpSA);

        // feed-forward network
        {
            // norm
            {
                cur = ggml_rms_norm(ctx0, inpFF);

                // cur = ffn_norm*cur
                cur = ggml_mul(ctx0,
                        ggml_repeat(ctx0, model.layers[il].ffn_norm, cur),
                        cur);
            }

            struct ggml_tensor * tmp = ggml_mul_mat(ctx0,
                    model.layers[il].w3,
                    cur);

            cur = ggml_mul_mat(ctx0,
                    model.layers[il].w1,
                    cur);

            // SILU activation
            cur = ggml_silu(ctx0, cur);

            cur = ggml_mul(ctx0, cur, tmp);

            cur = ggml_mul_mat(ctx0,
                    model.layers[il].w2,
                    cur);
        }

        cur = ggml_add(ctx0, cur, inpFF);

        // input for next layer
        inpL = cur;
    }

    lctx.use_buf(ctx0, 0);

    // used at the end to optionally extract the embeddings
    struct ggml_tensor * embeddings = NULL;

    // norm
    {

        inpL = ggml_rms_norm(ctx0, inpL);

        // inpL = norm*inpL
        inpL = ggml_mul(ctx0,
                    ggml_repeat(ctx0, model.norm, inpL),
                    inpL);

        embeddings = inpL;
    }

    // lm_head
    inpL = ggml_mul_mat(ctx0, model.output, inpL);

    lctx.use_buf(ctx0, -1);

    // logits -> probs
    //inpL = ggml_soft_max(ctx0, inpL);

    // run the computation
    ggml_build_forward_expand(&gf, inpL);
    ggml_graph_compute       (ctx0, &gf);

    // print timing information per ggml operation (for debugging purposes)
    // requires GGML_PERF to be defined
    //ggml_graph_print(&gf);

    // plot the computation graph in dot format (for debugging purposes)
    //if (n_past%100 == 0) {
    //    ggml_graph_dump_dot(&gf, NULL, "llama.dot");
    //}

    //embd_w.resize(n_vocab*N);
    //memcpy(embd_w.data(), ggml_get_data(inpL), sizeof(float)*n_vocab*N);

    // extract logits
    {
        auto & logits_out = lctx.logits;

        if (lctx.logits_all) {
            logits_out.resize(n_vocab * N);
            memcpy(logits_out.data(), (float *) ggml_get_data(inpL), sizeof(float)*n_vocab*N);
        } else {
            // return result for just the last token
            logits_out.resize(n_vocab);
            memcpy(logits_out.data(), (float *) ggml_get_data(inpL) + (n_vocab*(N-1)), sizeof(float)*n_vocab);
        }
    }

    // extract embeddings
    if (lctx.embedding.size()) {
        auto & embedding_out = lctx.embedding;

        embedding_out.resize(n_embd);
        memcpy(embedding_out.data(), (float *) ggml_get_data(embeddings) + (n_embd*(N - 1)), sizeof(float)*n_embd);
    }

    if (mem_per_token == 0) {
        mem_per_token = ggml_used_mem(ctx0)/N;
    }

#if 0
    printf("\n%s: used_mem = %.3f MB, scratch -- %.3f MB %.3f MB\n", __func__,
            ggml_used_mem(ctx0)/1024.0/1024.0,
            lctx.get_buf_max_mem(0)/1024.0/1024.0,
            lctx.get_buf_max_mem(1)/1024.0/1024.0);
#endif

    ggml_free(ctx0);

    // measure the performance only for the single-token evals
    if (N == 1) {
        lctx.t_eval_us += ggml_time_us() - t_start_us;
        lctx.n_eval++;
    }
    else if (N > 1) {
        lctx.t_p_eval_us += ggml_time_us() - t_start_us;
        lctx.n_p_eval += N;
    }

    return true;
}

//
// tokenizer
//

static size_t utf8_len(char src) {
    const size_t lookup[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4 };
    uint8_t highbits = static_cast<uint8_t>(src) >> 4;
    return lookup[highbits];
}

struct llama_sp_symbol {
    using index = int;
    index prev;
    index next;
    const char * text;
    size_t n;
};

struct llama_sp_bigram {
    struct comparator {
        bool operator()(llama_sp_bigram & l, llama_sp_bigram & r) {
            return (l.score < r.score) || (l.score == r.score && l.left > r.left);
        }
    };
    using queue_storage = std::vector<llama_sp_bigram>;
    using queue = std::priority_queue<llama_sp_bigram, queue_storage, comparator>;
    llama_sp_symbol::index left;
    llama_sp_symbol::index right;
    float score;
    size_t size;
};

// original implementation:
// https://github.com/ggerganov/llama.cpp/commit/074bea2eb1f1349a0118239c4152914aecaa1be4
struct llama_tokenizer {
    llama_tokenizer(const llama_vocab & vocab): vocab_(vocab) {}

    void tokenize(const std::string & text, std::vector<llama_vocab::id> & output) {
        // split string into utf8 chars
        int index = 0;
        size_t offs = 0;
        while (offs < text.size()) {
            llama_sp_symbol sym;
            size_t char_len = std::min(text.size() - offs, utf8_len(text[offs]));
            sym.text = text.c_str() + offs;
            sym.n = char_len;
            offs += char_len;
            sym.prev = index - 1;
            sym.next = offs == text.size() ? -1 : index + 1;
            index++;
            symbols_.emplace_back(std::move(sym));
        }

        // seed the work queue with all possible 2-character tokens.
        for (size_t i = 1; i < symbols_.size(); ++i) {
            try_add_bigram(i - 1, i);
        }

        // keep substituting the highest frequency pairs for as long as we can.
        while (!work_queue_.empty()) {
            auto bigram = work_queue_.top();
            work_queue_.pop();

            auto & left_sym = symbols_[bigram.left];
            auto & right_sym = symbols_[bigram.right];

            // if one of the symbols already got merged, skip it.
            if (left_sym.n == 0 || right_sym.n == 0 ||
                left_sym.n + right_sym.n != bigram.size) {
                continue;
            }

            // merge the right sym into the left one
            left_sym.n += right_sym.n;
            right_sym.n = 0;

            //printf("left = '%*s' size = %zu\n", (int) left_sym.n, left_sym.text, bigram.size);

            // remove the right sym from the chain
            left_sym.next = right_sym.next;
            if (right_sym.next >= 0) {
                symbols_[right_sym.next].prev = bigram.left;
            }

            // find more substitutions
            try_add_bigram(left_sym.prev, bigram.left);
            try_add_bigram(bigram.left, left_sym.next);
        }

        for (int i = 0; i != -1; i = symbols_[i].next) {
            auto & symbol = symbols_[i];
            auto token = vocab_.token_to_id.find(std::string(symbol.text, symbol.n));

            if (token == vocab_.token_to_id.end()) {
                // output any symbols that did not form tokens as bytes.
                for (int j = 0; j < (int) symbol.n; ++j) {
                    llama_vocab::id token_id = static_cast<uint8_t>(symbol.text[j]) + 3;
                    output.push_back(token_id);
                }
            } else {
                output.push_back((*token).second);
            }
        }
    }

private:
    void try_add_bigram(int left, int right) {
        if (left == -1 || right == -1) {
            return;
        }

        const std::string text = std::string(symbols_[left].text, symbols_[left].n + symbols_[right].n);
        auto token = vocab_.token_to_id.find(text);

        if (token == vocab_.token_to_id.end()) {
            return;
        }

        if (static_cast<size_t>((*token).second) >= vocab_.id_to_token.size()) {
            return;
        }

        const auto &tok_score = vocab_.id_to_token[(*token).second];

        llama_sp_bigram bigram;
        bigram.left = left;
        bigram.right = right;
        bigram.score = tok_score.score;
        bigram.size = text.size();
        work_queue_.push(bigram);
    }

    const llama_vocab & vocab_;
    std::vector<llama_sp_symbol> symbols_;
    llama_sp_bigram::queue work_queue_;
};

static std::vector<llama_vocab::id> llama_tokenize(const llama_vocab & vocab, const std::string & text, bool bos) {
    llama_tokenizer tokenizer(vocab);
    std::vector<llama_vocab::id> output;

    if (text.size() == 0) {
        return output;
    }

    if (bos) {
        output.push_back(1);
    }

    tokenizer.tokenize(text, output);
    return output;
}

//
// sampling
//

static void sample_top_k(std::vector<std::pair<float, llama_vocab::id>> & logits_id, int top_k) {
    // find the top k tokens
    std::partial_sort(
            logits_id.begin(),
            logits_id.begin() + top_k, logits_id.end(),
            [](const std::pair<float, llama_vocab::id> & a, const std::pair<float, llama_vocab::id> & b) {
        return a.first > b.first;
    });

    logits_id.resize(top_k);
}

static llama_vocab::id llama_sample_top_p_top_k(
        llama_context & lctx,
        const std::vector<llama_vocab::id> & last_n_tokens,
        int top_k,
        float top_p,
        float temp,
        float repeat_penalty) {
    auto & rng = lctx.rng;

    const int n_logits = lctx.model.hparams.n_vocab;

    const auto & logits = lctx.logits;
    const auto * plogits = logits.data() + logits.size() - n_logits;

    if (temp <= 0) {
        // select the token with the highest logit directly
        float max_logit = plogits[0];
        llama_vocab::id max_id = 0;

        for (int i = 1; i < n_logits; ++i) {
            if (plogits[i] > max_logit) {
                max_logit = plogits[i];
                max_id = i;
            }
        }
        return max_id;
    }

    std::vector<std::pair<float, llama_vocab::id>> logits_id;
    logits_id.reserve(n_logits);

    {
        const float scale = 1.0f/temp;
        for (int i = 0; i < n_logits; ++i) {
            // repetition penalty from ctrl paper (https://arxiv.org/abs/1909.05858)
            // credit https://github.com/facebookresearch/llama/compare/main...shawwn:llama:main
            if (std::find(last_n_tokens.begin(), last_n_tokens.end(), i) != last_n_tokens.end()) {
                // if score < 0 then repetition penalty has to multiplied to reduce the previous token probability
                if (plogits[i] < 0.0f) {
                    logits_id.push_back(std::make_pair(plogits[i]*scale*repeat_penalty, i));
                } else {
                    logits_id.push_back(std::make_pair(plogits[i]*scale/repeat_penalty, i));
                }
            } else {
                logits_id.push_back(std::make_pair(plogits[i]*scale, i));
            }
        }
    }

    sample_top_k(logits_id, top_k > 0 ? std::min(top_k, n_logits) : n_logits);

    // compute probs for the top k tokens
    std::vector<float> probs;
    probs.reserve(logits_id.size());

    float maxl = logits_id[0].first;
    double sum = 0.0;
    for (const auto & kv : logits_id) {
        const float p = expf(kv.first - maxl);
        probs.push_back(p);
        sum += p;
    }

    // normalize the probs
    for (auto & p : probs) {
        p /= sum;
    }

    if (top_p < 1.0) {
        double cumsum = 0.0;
        for (int i = 0; i < (int) probs.size(); i++) {
            cumsum += probs[i];
            if (cumsum >= top_p) {
                probs.resize(i + 1);
                logits_id.resize(i + 1);
                break;
            }
        }
    }

    //printf("\n");
    //for (int i = 0; i < (int) 10; i++) {
    //    printf("%d: '%s' %f\n", i, lctx.vocab.id_to_token.at(logits_id[i].second).tok.c_str(), probs[i]);
    //}
    //printf("\n\n");
    //exit(0);

    std::discrete_distribution<> dist(probs.begin(), probs.end());
    int idx = dist(rng);

    return logits_id[idx].second;
}

//
// quantization
//

static void llama_model_quantize_internal(const std::string & fname_inp, const std::string & fname_out, enum llama_ftype ftype) {
    ggml_type quantized_type;
    switch (ftype) {
        case LLAMA_FTYPE_MOSTLY_Q4_0: quantized_type = GGML_TYPE_Q4_0; break;
        case LLAMA_FTYPE_MOSTLY_Q4_1: quantized_type = GGML_TYPE_Q4_1; break;
        default: throw format("invalid output file type %d\n", ftype);
    };

    std::unique_ptr<llama_model_loader> model_loader(new llama_model_loader(fname_inp.c_str(), /*use_mmap*/ false,
                                                                            /*vocab_only*/ false));
    llama_file_saver file_saver(fname_out.c_str(), model_loader->file_loaders.at(0).get(), ftype);

    size_t total_size_org = 0;
    size_t total_size_new = 0;
    std::vector<int64_t> hist_all(1 << 4, 0);

    size_t idx = 0;
    for (llama_load_tensor & tensor : model_loader->tensors_map.tensors) {
        llama_buffer read_data;
        read_data.resize(tensor.size);
        tensor.data = read_data.addr;
        model_loader->load_data_for(tensor);

        printf("[%zu/%zu] %36s - %s, type = %6s, ",
               ++idx, model_loader->tensors_map.tensors.size(),
               tensor.name.c_str(), llama_format_tensor_shape(tensor.ne).c_str(),
               llama_format_type(tensor.type));

        // This used to be a regex, but <regex> has an extreme cost to compile times.
        bool quantize = tensor.name.rfind("weight") == tensor.name.size() - 6; // ends with 'weight'?

        // quantize only 2D tensors
        quantize &= (tensor.ne.size() == 2);

        enum ggml_type new_type;
        void * new_data;
        size_t new_size;
        llama_buffer work;

        if (!quantize) {
            new_type = tensor.type;
            new_data = tensor.data;
            new_size = tensor.size;
            printf("size = %8.3f MB\n", tensor.size/1024.0/1024.0);
        } else {
            new_type = quantized_type;
            float * f32_data;
            size_t nelements = tensor.ne.at(0) * tensor.ne.at(1);
            llama_buffer f32_conv_buf;
            if (tensor.type == GGML_TYPE_F32) {
                f32_data = (float *) tensor.data;
            } else if (tensor.type == GGML_TYPE_F16) {
                f32_conv_buf.resize(nelements * sizeof(float));
                f32_data = (float *) f32_conv_buf.addr;
                auto f16_data = (const ggml_fp16_t *) tensor.data;
                for (size_t i = 0; i < nelements; i++) {
                    f32_data[i] = ggml_fp16_to_fp32(f16_data[i]);
                }
            } else {
                throw format("type %s unsupported for integer quantization", llama_format_type(tensor.type));
            }

            printf("quantizing .. ");
            fflush(stdout);

            work.resize(nelements * 4); // upper bound on size
            new_data = work.addr;
            std::vector<int64_t> hist_cur(1 << 4, 0);

            switch (new_type) {
                case GGML_TYPE_Q4_0:
                    {
                        new_size = ggml_quantize_q4_0(f32_data, new_data, nelements, (int) tensor.ne.at(0), hist_cur.data());
                    } break;
                case GGML_TYPE_Q4_1:
                    {
                        new_size = ggml_quantize_q4_1(f32_data, new_data, nelements, (int) tensor.ne.at(0), hist_cur.data());
                    } break;
                default:
                    LLAMA_ASSERT(false);
            }

            printf("size = %8.2f MB -> %8.2f MB | hist: ", tensor.size/1024.0/1024.0, new_size/1024.0/1024.0);
            for (size_t i = 0; i < hist_cur.size(); i++) {
                hist_all[i] += hist_cur[i];
            }

            for (size_t i = 0; i < hist_cur.size(); i++) {
                printf("%5.3f ", hist_cur[i] / float(nelements));
            }
            printf("\n");
        }
        total_size_org += tensor.size;
        total_size_new += new_size;
        file_saver.write_tensor(tensor, new_type, new_data, new_size);
    }

    printf("%s: model size  = %8.2f MB\n", __func__, total_size_org/1024.0/1024.0);
    printf("%s: quant size  = %8.2f MB\n", __func__, total_size_new/1024.0/1024.0);

    {
        int64_t sum_all = 0;
        for (size_t i = 0; i < hist_all.size(); i++) {
            sum_all += hist_all[i];
        }

        printf("%s: hist: ", __func__);
        for (size_t i = 0; i < hist_all.size(); i++) {
            printf("%5.3f ", hist_all[i] / float(sum_all));
        }
        printf("\n");
    }
}

//
// interface implementation
//

struct llama_context * llama_init_from_file(
                             const char * path_model,
            struct llama_context_params   params) {
    ggml_time_init();

    llama_context * ctx = new llama_context;

    if (params.seed <= 0) {
        params.seed = time(NULL);
    }

    unsigned cur_percentage = 0;
    if (params.progress_callback == NULL) {
        params.progress_callback_user_data = &cur_percentage;
        params.progress_callback = [](float progress, void * ctx) {
            unsigned * cur_percentage_p = (unsigned *) ctx;
            unsigned percentage = (unsigned) (100 * progress);
            while (percentage > *cur_percentage_p) {
                ++*cur_percentage_p;
                fprintf(stderr, ".");
                fflush(stderr);
                if (percentage >= 100) {
                    fprintf(stderr, "\n");
                }
            }
        };
    }

    ctx->rng = std::mt19937(params.seed);
    ctx->logits_all = params.logits_all;

    ggml_type memory_type = params.f16_kv ? GGML_TYPE_F16 : GGML_TYPE_F32;

    if (!llama_model_load(path_model, *ctx, params.n_ctx, memory_type,
                          params.use_mmap, params.use_mlock, params.vocab_only,
                          params.progress_callback, params.progress_callback_user_data)) {
        fprintf(stderr, "%s: failed to load model\n", __func__);
        llama_free(ctx);
        return nullptr;
    }

    // reserve memory for context buffers
    if (!params.vocab_only) {
        if (!kv_cache_init(ctx->model.hparams, ctx->model.kv_self, memory_type, ctx->model.hparams.n_ctx)) {
            fprintf(stderr, "%s: kv_cache_init() failed for self-attention cache\n", __func__);
            llama_free(ctx);
            return nullptr;
        }

        {
            const size_t memory_size = ggml_nbytes(ctx->model.kv_self.k) + ggml_nbytes(ctx->model.kv_self.v);
            fprintf(stderr, "%s: kv self size  = %7.2f MB\n", __func__, memory_size / 1024.0 / 1024.0);
        }

        const auto & hparams = ctx->model.hparams;

        // resized during inference
        if (params.logits_all) {
            ctx->logits.reserve(hparams.n_ctx*hparams.n_vocab);
        } else {
            ctx->logits.reserve(hparams.n_vocab);
        }

        if (params.embedding){
            ctx->embedding.resize(hparams.n_embd);
        }

        ctx->buf_compute.resize(MEM_REQ_EVAL.at(ctx->model.type));

        ctx->buf_scratch[0].resize(MEM_REQ_SCRATCH0.at(ctx->model.type));
        ctx->buf_scratch[1].resize(MEM_REQ_SCRATCH1.at(ctx->model.type));
    }

    return ctx;
}

void llama_free(struct llama_context * ctx) {
    delete ctx;
}

int llama_model_quantize(
        const char * fname_inp,
        const char * fname_out,
  enum llama_ftype   ftype) {
    try {
        llama_model_quantize_internal(fname_inp, fname_out, ftype);
        return 0;
    } catch (const std::string & err) {
        fprintf(stderr, "%s: failed to quantize: %s\n", __func__, err.c_str());
        return 1;
    }
}

// Returns the KV cache that will contain the context for the
// ongoing prediction with the model.
const uint8_t * llama_get_kv_cache(struct llama_context * ctx) {
    return ctx->model.kv_self.buf.addr;
}

// Returns the size of the KV cache
size_t llama_get_kv_cache_size(struct llama_context * ctx) {
    return ctx->model.kv_self.buf.size;
}

int llama_get_kv_cache_token_count(struct llama_context * ctx) {
    return ctx->model.kv_self.n;
}

// Sets the KV cache containing the current context for the model
void llama_set_kv_cache(
        struct llama_context * ctx,
               const uint8_t * kv_cache,
                      size_t   n_size,
                         int   n_token_count) {
    // Make sure we have the same kv cache setup
    LLAMA_ASSERT(ctx->model.kv_self.buf.size == n_size);
    void * k_data = ctx->model.kv_self.k->data; // remember data pointers
    void * v_data = ctx->model.kv_self.v->data; // because their value is stored in buf and overwritten by memcpy
    memcpy(ctx->model.kv_self.buf.addr, kv_cache, n_size);
    ctx->model.kv_self.k->data = k_data; // restore correct data pointers
    ctx->model.kv_self.v->data = v_data;
    ctx->model.kv_self.n = n_token_count;
}

int llama_eval(
        struct llama_context * ctx,
           const llama_token * tokens,
                         int   n_tokens,
                         int   n_past,
                         int   n_threads) {
    if (!llama_eval_internal(*ctx, tokens, n_tokens, n_past, n_threads)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }
    // get a more accurate load time, upon first eval
    if (!ctx->has_evaluated_once) {
        ctx->t_load_us = ggml_time_us() - ctx->t_start_us;
        ctx->has_evaluated_once = true;
    }
    return 0;
}

int llama_tokenize(
        struct llama_context * ctx,
                  const char * text,
                 llama_token * tokens,
                         int   n_max_tokens,
                        bool   add_bos) {
    auto res = llama_tokenize(ctx->vocab, text, add_bos);

    if (n_max_tokens < (int) res.size()) {
        fprintf(stderr, "%s: too many tokens\n", __func__);
        return -((int) res.size());
    }

    for (size_t i = 0; i < res.size(); i++) {
        tokens[i] = res[i];
    }

    return res.size();
}

int llama_n_vocab(struct llama_context * ctx) {
    return ctx->vocab.id_to_token.size();
}

int llama_n_ctx(struct llama_context * ctx) {
    return ctx->model.hparams.n_ctx;
}

int llama_n_embd(struct llama_context * ctx) {
    return ctx->model.hparams.n_embd;
}

float * llama_get_logits(struct llama_context * ctx) {
    return ctx->logits.data();
}

float * llama_get_embeddings(struct llama_context * ctx) {
    return ctx->embedding.data();
}

const char * llama_token_to_str(struct llama_context * ctx, llama_token token) {
    if (token >= llama_n_vocab(ctx)) {
        return nullptr;
    }

    return ctx->vocab.id_to_token[token].tok.c_str();
}

llama_token llama_token_bos() {
    return 1;
}

llama_token llama_token_eos() {
    return 2;
}

llama_token llama_sample_top_p_top_k(
          llama_context * ctx,
      const llama_token * last_n_tokens_data,
                    int   last_n_tokens_size,
                    int   top_k,
                  float   top_p,
                  float   temp,
                  float   repeat_penalty) {
    const int64_t t_start_sample_us = ggml_time_us();

    llama_token result = 0;

    // TODO: avoid this ...
    const auto last_n_tokens = std::vector<llama_token>(last_n_tokens_data, last_n_tokens_data + last_n_tokens_size);

    result = llama_sample_top_p_top_k(
            *ctx,
            last_n_tokens,
            top_k,
            top_p,
            temp,
            repeat_penalty);

    ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    ctx->n_sample++;

    return result;
}


void llama_print_timings(struct llama_context * ctx) {
    const int64_t t_end_us = ggml_time_us();

    const int32_t n_sample = std::max(1, ctx->n_sample);
    const int32_t n_eval   = std::max(1, ctx->n_eval);
    const int32_t n_p_eval = std::max(1, ctx->n_p_eval);

    fprintf(stderr, "\n");
    fprintf(stderr, "%s:        load time = %8.2f ms\n", __func__, ctx->t_load_us / 1000.0);
    fprintf(stderr, "%s:      sample time = %8.2f ms / %5d runs   (%8.2f ms per run)\n",   __func__, 1e-3 * ctx->t_sample_us, n_sample, 1e-3 * ctx->t_sample_us / n_sample);
    fprintf(stderr, "%s: prompt eval time = %8.2f ms / %5d tokens (%8.2f ms per token)\n", __func__, 1e-3 * ctx->t_p_eval_us, n_p_eval, 1e-3 * ctx->t_p_eval_us / n_p_eval);
    fprintf(stderr, "%s:        eval time = %8.2f ms / %5d runs   (%8.2f ms per run)\n",   __func__, 1e-3 * ctx->t_eval_us,   n_eval,   1e-3 * ctx->t_eval_us   / n_eval);
    fprintf(stderr, "%s:       total time = %8.2f ms\n", __func__, (t_end_us - ctx->t_start_us)/1000.0);
}

void llama_reset_timings(struct llama_context * ctx) {
    ctx->t_start_us = ggml_time_us();
    ctx->t_sample_us = ctx->n_sample = 0;
    ctx->t_eval_us   = ctx->n_eval   = 0;
    ctx->t_p_eval_us = ctx->n_p_eval = 0;
}

const char * llama_print_system_info(void) {
    static std::string s;

    s  = "";
    s += "AVX = "       + std::to_string(ggml_cpu_has_avx())       + " | ";
    s += "AVX2 = "      + std::to_string(ggml_cpu_has_avx2())      + " | ";
    s += "AVX512 = "    + std::to_string(ggml_cpu_has_avx512())    + " | ";
    s += "FMA = "       + std::to_string(ggml_cpu_has_fma())       + " | ";
    s += "NEON = "      + std::to_string(ggml_cpu_has_neon())      + " | ";
    s += "ARM_FMA = "   + std::to_string(ggml_cpu_has_arm_fma())   + " | ";
    s += "F16C = "      + std::to_string(ggml_cpu_has_f16c())      + " | ";
    s += "FP16_VA = "   + std::to_string(ggml_cpu_has_fp16_va())   + " | ";
    s += "WASM_SIMD = " + std::to_string(ggml_cpu_has_wasm_simd()) + " | ";
    s += "BLAS = "      + std::to_string(ggml_cpu_has_blas())      + " | ";
    s += "SSE3 = "      + std::to_string(ggml_cpu_has_sse3())      + " | ";
    s += "VSX = "       + std::to_string(ggml_cpu_has_vsx())       + " | ";

    return s.c_str();
}

// For internal test use
std::vector<std::pair<std::string, struct ggml_tensor *>>& llama_internal_get_tensor_map(struct llama_context * ctx) {
    return ctx->model.tensors_by_name;
}

// Returns the size of the state
size_t llama_get_state_size(struct llama_context * ctx) {
    const size_t s_bool = sizeof(int32_t);
    // we don't know size of rng until we actually serialize it. so reserve more than enough memory for its serialized state. 
    // for reference, std::mt19937(1337) serializes to 6701 bytes.
    const size_t s_rng_size = sizeof(size_t); 
    const size_t s_rng = 64*1024; 
    const size_t s_logits_capacity = sizeof(size_t);
    const size_t s_logits_size = sizeof(size_t);
    const size_t s_logits = ctx->logits.capacity() * sizeof(float);
    const size_t s_embedding_size = sizeof(size_t);
    const size_t s_embedding = ctx->embedding.size() * sizeof(float);
    const size_t s_kv_size = sizeof(size_t);
    const size_t s_kv_ntok = sizeof(int);
    const size_t s_kv = llama_get_kv_cache_size(ctx);
    const size_t s_total = (
        + s_rng_size
        + s_rng
        + s_logits_capacity
        + s_logits_size
        + s_logits
        + s_embedding_size
        + s_embedding
        + s_kv_size
        + s_kv_ntok
        + s_kv
    );
    return s_total;
}

// Copies the state to the specified destination address
size_t llama_copy_state_data(struct llama_context * ctx, uint8_t * dest) {
    std::stringstream rng_ss;
    rng_ss << ctx->rng;
    const size_t rng_size = rng_ss.str().size();
    char rng_buf[64*1024];
    memset(&rng_buf[0], 0, 64*1024);
    memcpy(&rng_buf[0], rng_ss.str().data(), rng_ss.str().size());
    const int32_t has_evaluated_once = ctx->has_evaluated_once ? 1 : 0;
    const int32_t logits_all = ctx->logits_all ? 1 : 0;
    const size_t logits_capacity = ctx->logits.capacity();
    const size_t logits_size = ctx->logits.size();
    const size_t embedding_size = ctx->embedding.size();
    const size_t kv_size = llama_get_kv_cache_size(ctx);
    const int kv_ntok = llama_get_kv_cache_token_count(ctx);

    uint8_t * out = dest;
    memcpy(out, &rng_size, sizeof(size_t)); out += sizeof(size_t);
    memcpy(out, &rng_buf[0], 64*1024); out += 64*1024;
    memcpy(out, &logits_capacity, sizeof(size_t)); out += sizeof(size_t);
    memcpy(out, &logits_size, sizeof(size_t)); out += sizeof(size_t);
    if (logits_size) {
        memcpy(out, ctx->logits.data(), logits_size * sizeof(float)); 
    }
    out += logits_capacity * sizeof(float);
    memcpy(out, &embedding_size, sizeof(size_t)); out += sizeof(size_t);
    if (embedding_size) {
        memcpy(out, ctx->embedding.data(), embedding_size * sizeof(float)); out += embedding_size * sizeof(float);
    }
    memcpy(out, &kv_size, sizeof(size_t)); out += sizeof(size_t);
    memcpy(out, &kv_ntok, sizeof(int)); out += sizeof(int);
    if (kv_size) {
        memcpy(out, llama_get_kv_cache(ctx), kv_size); out += kv_size;
    }
    const size_t written = out - dest;
    const size_t expected = llama_get_state_size(ctx);
    LLAMA_ASSERT(written == expected);
    return written;
}

// Copies the state to the specified destination address
size_t llama_set_state_data(struct llama_context * ctx, const uint8_t * src) {
    size_t rng_size;
    char rng_buf[64*1024];
    std::stringstream rng_ss;

    const uint8_t * in = src;
    memcpy(&rng_size, in, sizeof(size_t)); in += sizeof(size_t);
    memcpy(&rng_buf[0], in, 64*1024); in += 64*1024;
    rng_ss.str(std::string(&rng_buf[0], rng_size));
    rng_ss >> ctx->rng;
    LLAMA_ASSERT(rng_ss.fail() == false);

    int32_t has_evaluated_once;
    int32_t logits_all;
    size_t logits_capacity;
    size_t logits_size;
    size_t embedding_size;
    size_t kv_size;
    int kv_ntok;

    memcpy(&logits_capacity, in, sizeof(size_t)); in += sizeof(size_t);
    memcpy(&logits_size, in, sizeof(size_t)); in += sizeof(size_t);
    LLAMA_ASSERT(ctx->logits.capacity() == logits_capacity);
    if (logits_size) {
        ctx->logits.resize(logits_size);
        memcpy(ctx->logits.data(), in, logits_size * sizeof(float)); 
    }
    in += logits_capacity * sizeof(float);
    memcpy(&embedding_size, in, sizeof(size_t)); in += sizeof(size_t);
    LLAMA_ASSERT(ctx->embedding.capacity() == embedding_size);
    if (embedding_size) {
        memcpy(ctx->embedding.data(), in, embedding_size * sizeof(float)); 
        in += embedding_size * sizeof(float);
    }
    memcpy(&kv_size, in, sizeof(size_t)); in += sizeof(size_t);
    memcpy(&kv_ntok, in, sizeof(int)); in += sizeof(int);
    if (kv_size) {
        LLAMA_ASSERT(ctx->model.kv_self.buf.size == kv_size);
        void * k_data = ctx->model.kv_self.k->data; // remember data pointers
        void * v_data = ctx->model.kv_self.v->data; // because their value is stored in buf and overwritten by memcpy
        memcpy(ctx->model.kv_self.buf.addr, in, kv_size); 
        ctx->model.kv_self.k->data = k_data; // restore correct data pointers
        ctx->model.kv_self.v->data = v_data;
        in += kv_size;
    }
    ctx->model.kv_self.n = kv_ntok;
    const size_t nread = in - src;
    const size_t expected = llama_get_state_size(ctx);
    LLAMA_ASSERT(nread == expected);
    return nread;
}
