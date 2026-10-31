#pragma once
#include "tables.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// ── Candidate index file ──────────────────────────────────────────────────────
//
// A text header padded to INDEX_HEADER_BYTES, then one 8-byte record per candidate, sorted
// by score (rank 1 = most probable).  A record holds the key length in its top 6 bits and
// the key packed base 26 below; plaintext, word split and score are recomputed from the
// key when read, so rank r is at byte INDEX_HEADER_BYTES + 8·(r − 1).
//
// Header lines:  VIGIDX 1 / cipher <text> / dict <fingerprint> <words> /
//                max_key_length <n> / count <n> / kl <len> <count> ... / note <text> ... / end

static constexpr size_t INDEX_HEADER_BYTES = 4096;

struct IndexOptions {
    int    threads        = 0;              // 0 = all hardware threads
    int    max_key_length = 12;
    // Candidates the index may hold in total.  Building needs about 28 bytes of memory per
    // candidate, so the default keeps the peak near 4 GB.  A key length that would exceed
    // it is skipped with a note: with that many readings it carries almost no information.
    size_t max_candidates = 150'000'000;
};

struct IndexSummary {
    size_t count = 0;
    std::vector<std::pair<int, size_t>> per_key_length;   // (key length, candidates)
    std::vector<std::string> notes;                       // e.g. key lengths skipped
};

// Full-vocabulary search over key lengths 1..min(max_key_length, len − 1); writes the index.
// Progress lines go to stderr.  Throws std::runtime_error on failure.
IndexSummary build_index(const std::string& cipher, const Dictionary& dict,
                         const IndexOptions& opt, const std::string& path);

struct Candidate {
    size_t      rank;
    std::string key, plain, words;
    double      score;
};

class IndexReader {
public:
    // Throws std::runtime_error if the file is missing, malformed or was built with a
    // different dictionary.
    IndexReader(const std::string& path, const Dictionary& dict);
    ~IndexReader();
    IndexReader(const IndexReader&) = delete;
    IndexReader& operator=(const IndexReader&) = delete;

    const std::string&  cipher()  const { return cipher_; }
    const IndexSummary& summary() const { return summary_; }

    // Candidates of ranks [first, last] (1-based, clipped to the count).
    std::vector<Candidate> range(size_t first, size_t last);
    // Rank of the candidate with this plaintext, or 0 if it is not in the index.
    size_t rank_of(const std::string& plain);

private:
    Candidate decode(size_t rank, uint64_t record) const;

    const Dictionary& dict_;
    FILE*             f_ = nullptr;
    std::string       cipher_;
    IndexSummary      summary_;
};
