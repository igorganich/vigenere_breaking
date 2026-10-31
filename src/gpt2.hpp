#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ── GPT-2 small, for rescoring the best candidates ────────────────────────────
//
// A self-contained CPU implementation of OpenAI's GPT-2 (124M parameters) that reads the
// Hugging Face release as it is: model.safetensors, vocab.json and merges.txt in one
// directory (tools/get_gpt2.sh downloads them).  It does one thing: the total
// log-probability of short texts, log P(text) = Σ log P(token | previous tokens), with the
// end-of-text token as the context of the first token.
//
// Results do not depend on the number of threads or on which texts share a batch: every
// value is computed in a fixed order.  Matrix products use AVX2/FMA when the build targets
// a CPU that has them, and portable C++ otherwise.

class Gpt2 {
public:
    // True if `dir` holds the three files.
    static bool available(const std::string& dir);

    // Loads the model; throws std::runtime_error if a file is missing or malformed.
    explicit Gpt2(const std::string& dir);

    // GPT-2 byte-level BPE token ids of `text` (ASCII only; throws otherwise).
    std::vector<int> encode(const std::string& text) const;

    // Total log P(text) in nats for each text.
    std::vector<double> logprob(const std::vector<std::string>& texts, int threads) const;

    struct Packed {                       // a K x N weight matrix in panels of 24 columns
        int K = 0, N = 0, panels = 0;
        std::vector<float> data;          // [panel][k][24], zero-padded
        std::vector<float> bias;          // [panel * 24], zero-padded (empty: no bias)
    };

private:
    struct Layer {
        std::vector<float> ln1_g, ln1_b, ln2_g, ln2_b;
        Packed attn, proj, fc, out;       // c_attn, attn.c_proj, mlp.c_fc, mlp.c_proj
    };

    std::vector<double> forward(const std::vector<std::vector<int>>& seqs, int threads) const;

    int n_layer_ = 12, n_head_ = 12, n_embd_ = 768, n_vocab_ = 0, n_ctx_ = 0;
    int eot_ = 0;                                      // <|endoftext|>
    std::vector<float> wte_, wpe_, lnf_g_, lnf_b_;     // embeddings, final layer norm
    Packed head_;                                      // wte transposed: hidden -> logits
    std::vector<Layer> layers_;

    std::unordered_map<std::string, int> vocab_;       // byte-level token -> id
    std::unordered_map<std::string, int> merge_rank_;  // "a b" -> merge priority
    std::string byte_symbol_[256];                     // byte -> its UTF-8 symbol
};
