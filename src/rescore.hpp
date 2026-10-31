#pragma once
#include "gpt2.hpp"
#include "index.hpp"
#include <string>
#include <vector>

// ── Stage 3: rescoring the best candidates with GPT-2 ─────────────────────────
//
// The trigram model orders every candidate; GPT-2 reads the best `top` of them as whole
// sentences ("I am told to go and see it by four.", from the trigram's word split) and
// ranks them by
//
//     score = log P_GPT-2(sentence) − key_length · ln 26
//
// with ties broken by trigram rank.  The result is a shortlist stored next to the index
// in <index>.lm; the index itself keeps the trigram order.

struct Rescored {
    size_t      trigram_rank = 0;
    std::string key, sentence;
    double      score = 0;
};

// The sentence GPT-2 reads for a plaintext and its trigram word split ("[i:11][am:209]...").
std::string lm_sentence(const std::string& plain, const std::string& words);

// The best `top` candidates of the index, rescored and sorted (best first).
std::vector<Rescored> rescore(IndexReader& idx, const Gpt2& lm, size_t top, int threads);

std::string shortlist_path(const std::string& index_path);

// Write / read <index>.lm.  The file records the ciphertext, candidate count and ranking
// model of its index; reading throws std::runtime_error if they do not match `idx` (the
// index was rebuilt or re-ranked since) or the file is missing.
void write_shortlist(const std::string& index_path, const IndexReader& idx, const std::vector<Rescored>& list);
std::vector<Rescored> read_shortlist(const std::string& index_path, const IndexReader& idx);
