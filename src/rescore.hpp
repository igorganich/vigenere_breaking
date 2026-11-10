#pragma once
#include "gpt2.hpp"
#include "index.hpp"
#include <functional>
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

// The sentence GPT-2 reads for a plaintext and its trigram word split ("[i:11][am:209]..."):
// words in their spelling for GPT-2 (Dictionary::display, German "schön") if `dict` has one,
// English "i" as "I", a capital first letter and a full stop.
std::string lm_sentence(const std::string& plain, const std::string& words, const Dictionary* dict = nullptr);

// The best `top` candidates of the index, rescored and sorted (best first).
std::vector<Rescored> rescore(IndexReader& idx, const Gpt2& lm, size_t top, int threads);

// Rescore the candidates at `ranks` (trigram ranks, any order) in batches of `batch`, and
// append each batch to `scores_path` as "rank<TAB>key<TAB>score<TAB>sentence" lines as soon
// as it is done.  Ranks already in the file are skipped, so an interrupted run resumes where
// it stopped.  `progress(done, total)` is called after every batch.  Returns every score in
// the file, sorted best first.
std::vector<Rescored> rescore_ranks(IndexReader& idx, const Gpt2& lm, const std::vector<size_t>& ranks,
                                    const std::string& scores_path, int threads, size_t batch,
                                    const std::function<void(size_t, size_t)>& progress);

std::string shortlist_path(const std::string& index_path);

// Write / read <index>.lm.  The file records the ciphertext, candidate count and ranking
// model of its index; reading throws std::runtime_error if they do not match `idx` (the
// index was rebuilt or re-ranked since) or the file is missing.
void write_shortlist(const std::string& index_path, const IndexReader& idx, const std::vector<Rescored>& list);
std::vector<Rescored> read_shortlist(const std::string& index_path, const IndexReader& idx);
