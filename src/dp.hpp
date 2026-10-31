#pragma once
#include "tables.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Keys are packed base 26, first letter lowest: code = Σ (key[i] - 'a') · 26^i.
uint64_t    pack_key(const std::string& key);
std::string unpack_key(uint64_t code, int key_len);

struct SearchStats {
    size_t peak_states = 0;      // largest number of DP states in one column (0 for brute force)
    bool   over_limit  = false;  // stopped: more than `limit` keys
};

// Every key of length key_len (1 <= key_len < cipher.size() <= 25) whose decryption splits
// completely into dictionary words, each key exactly once, packed with pack_key.
// Key lengths <= 4 use a brute force over all 26^key_len keys; longer ones the column DP.
// Stops and returns {} with stats->over_limit set once more than `limit` keys are found.
std::vector<uint64_t> find_keys(const std::string& cipher, int key_len, const Dictionary& dict,
                                int threads, size_t limit, SearchStats* stats = nullptr);

// Run fn(i) for i in [0, n) on `threads` threads, handing out indexes in chunks.
void parallel_for(size_t n, int threads, const std::function<void(size_t)>& fn);
