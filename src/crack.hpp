#pragma once
#include "tables.hpp"
#include "cipher.hpp"
#include <string>
#include <vector>

struct CrackCandidate {
    std::string plain;
    std::string key;
    double      fitness;   // avg tetragram log-prob (higher = more English-like)
    WordSegment segment;   // non-overlapping word split + coverage
};

// Parameters for word_cover_search (CSP backtracking).
// 0 means unlimited for results and budget; -1 for min_word means use default (kl-1).
struct WcsParams {
    int min_word = -1;       // minimum first-word length (-1 = kl-1 heuristic)
    int results  = 500;      // max results per key length and first-word length (0 = unlimited)
    int budget   = 500'000;  // max recursion nodes per key length (0 = unlimited)
};

// Longest ciphertext accepted: CSP branching grows quickly with length.
static constexpr int MAX_CIPHER_LEN = 25;

// Runs CSP word-cover search for every key length 1..12 and returns candidates
// with ≥ 90% word coverage, sorted by:
//   coverage ↓ → key length ↑ → word count ↑ → word rank ↑
std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& tables,
                                  WcsParams wcs = {});
