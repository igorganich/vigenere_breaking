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
    int results  = 500;      // max results per key length (0 = unlimited)
    int budget   = 500'000;  // max recursion nodes per key length (0 = unlimited)
};

// Controls which key-length strategies crack() runs and how results are ranked.
enum class KeyPriority {
    BOTH,       // default: run both short-key and long-key searches fully
    LONG_FIRST, // run long-key; skip short-key when long-key found high-coverage results
    SHORT_ONLY, // run short-key only (skip long-key search)
    EXHAUSTIVE, // no early exits: try every kl, collect all candidates,
                // sort by avg word-frequency rank (lower = more common = more likely correct)
};

// Returns ALL candidates found across all key lengths and methods,
// sorted by non-overlapping word coverage (highest first).
std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& tables,
                                  KeyPriority priority = KeyPriority::BOTH,
                                  WcsParams wcs = {});
