#pragma once
#include "tables.hpp"
#include "cipher.hpp"
#include <string>
#include <vector>

struct CrackCandidate {
    std::string plain;
    std::string key;
    double      fitness;   // avg tetragram log-prob (higher = more English-like)
    WordSegment segment;   // most probable word split + coverage
    double      score;     // log P(plain) + log P(key): segment.lm − key_len·ln 26
};

// Search parameters.  0 means unlimited for results, budget and time_limit.
struct WcsParams {
    int    min_word   = 1;       // shortest first word to try; never stricter than kl-1
    int    results    = 500;     // max results per key length and first-word length
    int    budget     = 500'000; // max word placements per key length and first-word length
    double time_limit = 300;     // seconds (0 = none); stop and return what was found so far
    int    top        = 10'000;  // best candidates kept and returned (0 = all)
    int    threads    = 0;       // worker threads (0 = all hardware threads)
    double progress   = 0;       // seconds between progress lines on stderr (0 = none)
};

// One search bucket: all coverings for key length kl whose first word has L letters.
struct BucketStats {
    enum class Status { NotRun, Complete, TimeLimit, Budget, ResultCap };

    int    kl = 0, L = 0, stage = 0;
    Status status     = Status::NotRun;
    int    n_words    = 0;     // first words of length L in the dictionary
    int    tried      = 0;     // first words fully searched (always the most common ones)
    double mass_tried = 0;     // share of the bucket's word probability tried
    double seconds    = 0;     // time spent searching this bucket

    // Calibration: estimated time to search every first word of this bucket with
    // no caps, from a uniform random sample of first words (< 0 = not sampled).
    // est_lower_bound when a sample was cut short and the estimate may be low.
    double est_full        = -1;
    bool   est_lower_bound = false;
    int    samples         = 0;

    // Estimated time to search the first words not yet tried, with no caps
    // (0 once complete; < 0 if unknown).
    double est_left() const {
        if (status == Status::Complete) return 0;
        if (est_full < 0 || n_words == 0) return -1;
        return est_full * (n_words - tried) / n_words;
    }
};

// What the search got through, for the coverage report.
struct CrackStats {
    bool   timed_out   = false;
    double calibration = 0;    // seconds spent sampling buckets
    double search      = 0;    // seconds spent in the search proper
    int    threads     = 1;    // worker threads used
    std::vector<BucketStats> buckets; // in run order
};

// Longest ciphertext accepted: CSP branching grows quickly with length.
static constexpr int MAX_CIPHER_LEN = 25;

// Runs CSP word-cover search for every key length 1..12 and returns the best
// wcs.top distinct plaintexts with ≥ 90% word coverage, most probable first.
std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& tables,
                                  WcsParams wcs = {}, CrackStats* stats = nullptr);
