#pragma once
#include "tables.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Keys are packed base A (the alphabet size), first letter lowest: code = Σ letter(key[i]) · A^i.
uint64_t    pack_key(const std::string& key, int alpha);
std::string unpack_key(uint64_t code, int key_len, int alpha);

struct SearchStats {
    size_t peak_states = 0;      // largest number of DP states in one column (0 for brute force)
    bool   over_limit  = false;  // stopped: more than `limit` keys
};

// Receives found keys in batches.  Called from worker threads, possibly concurrently, so it
// must be thread-safe; an exception it throws stops the search and is rethrown by find_keys.
using KeySink = std::function<void(const uint64_t* keys, size_t n)>;

// Every key of length key_len (1 <= key_len < cipher.size() <= 25) whose decryption splits
// completely into dictionary words, each key exactly once and in no particular order, packed
// with pack_key and handed to `sink`.  Key lengths <= 4 use a brute force over all
// A^key_len keys; longer ones the column DP.  Returns the number of keys.  Once more than
// `limit` keys are found it stops, sets stats->over_limit and returns 0; keys already handed
// to the sink must then be discarded by the caller.
size_t find_keys(const std::string& cipher, int key_len, const Dictionary& dict, int threads,
                 size_t limit, const KeySink& sink, SearchStats* stats = nullptr);

// The same, collected into a vector ({} when over the limit).
std::vector<uint64_t> find_keys(const std::string& cipher, int key_len, const Dictionary& dict,
                                int threads, size_t limit, SearchStats* stats = nullptr);

// Run fn(i) for i in [0, n) on `threads` threads, handing out indexes in chunks.
void parallel_for(size_t n, int threads, const std::function<void(size_t)>& fn);
