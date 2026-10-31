#include "index.hpp"
#include "cipher.hpp"
#include "dp.hpp"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

inline uint64_t make_record(int kl, uint64_t code) { return (uint64_t)kl << 58 | code; }
inline int      record_kl(uint64_t r)   { return (int)(r >> 58); }
inline uint64_t record_code(uint64_t r) { return r & ((1ull << 58) - 1); }

std::string hex(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

// Shortest period of the key once expanded over n letters.  A key shorter than its own
// length in this sense (e.g. "abab" over any text) gives the same plaintext as the shorter
// key, which is already a candidate, so it is dropped.
int expanded_period(const std::string& key, int n)
{
    const int kl = (int)key.size();
    for (int d = 1; d < kl; d++) {
        bool same = true;
        for (int i = d; i < n && same; i++) same = key[i % kl] == key[(i - d) % kl];
        if (same) return d;
    }
    return kl;
}

} // namespace

IndexSummary build_index(const std::string& cipher, const Dictionary& dict,
                         const IndexOptions& opt, const std::string& path)
{
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    const auto start = Clock::now();
    const int  n = (int)cipher.size();
    const int  threads = opt.threads > 0 ? opt.threads : std::max(1, (int)std::thread::hardware_concurrency());

    IndexSummary sum;
    std::vector<uint64_t> records;
    for (int kl = 1; kl <= std::min(opt.max_key_length, n - 1); kl++) {
        // If the ciphertext repeats with period kl, so must the plaintext, and any repeating
        // English text is an equally good answer: this key length carries no information.
        bool repeats = true;
        for (int i = 0; i + kl < n && repeats; i++) repeats = cipher[i] == cipher[i + kl];
        if (repeats) {
            sum.notes.push_back("key length " + std::to_string(kl) +
                                " skipped: the ciphertext repeats with this period");
            std::fprintf(stderr, "key length %2d: skipped (ciphertext repeats with this period)\n", kl);
            continue;
        }
        auto t0 = Clock::now();
        SearchStats stats;
        const size_t room = opt.max_candidates - std::min(opt.max_candidates, records.size());
        std::vector<uint64_t> keys = find_keys(cipher, kl, dict, threads, room, &stats);
        if (stats.over_limit) {
            sum.notes.push_back("key length " + std::to_string(kl) + " skipped: more than " +
                                std::to_string(room) + " candidates (too ambiguous to list)");
            std::fprintf(stderr, "key length %2d: skipped, more than %zu candidates (%.1fs)\n", kl, room, since(t0));
            continue;
        }
        size_t kept = 0;
        for (uint64_t code : keys)
            if (expanded_period(unpack_key(code, kl), n) == kl) { records.push_back(make_record(kl, code)); kept++; }
        std::vector<uint64_t>().swap(keys);
        sum.per_key_length.push_back({kl, kept});
        std::fprintf(stderr, "key length %2d: %zu candidates (%.1fs)\n", kl, kept, since(t0));
    }
    sum.count = records.size();

    auto t0 = Clock::now();
    // Scores in double, exactly as the reader recomputes them, so the stored order is the
    // order a reader sees.
    std::vector<double> score(records.size());
    parallel_for(records.size(), threads, [&](size_t i){
        int kl = record_kl(records[i]);
        score[i] = candidate_score(dict, vig_decrypt(cipher, unpack_key(record_code(records[i]), kl)), kl);
    });
    std::vector<uint32_t> order(records.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){
        return score[a] != score[b] ? score[a] > score[b] : records[a] < records[b];
    });
    std::fprintf(stderr, "ranked %zu candidates (%.1fs)\n", records.size(), since(t0));

    std::string h = "VIGIDX 1\ncipher " + cipher + "\ndict " + hex(dict.fingerprint) + " " +
                    std::to_string(dict.words.size()) + "\nmax_key_length " +
                    std::to_string(opt.max_key_length) + "\ncount " + std::to_string(sum.count) + "\n";
    for (auto [kl, c] : sum.per_key_length) h += "kl " + std::to_string(kl) + " " + std::to_string(c) + "\n";
    for (const auto& note : sum.notes) h += "note " + note + "\n";
    h += "end\n";
    if (h.size() > INDEX_HEADER_BYTES) throw std::runtime_error("index header too long");
    h.resize(INDEX_HEADER_BYTES, '\n');

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size();
    std::vector<uint64_t> buf;
    buf.reserve(1 << 16);
    auto flush = [&]{ ok = ok && std::fwrite(buf.data(), 8, buf.size(), f) == buf.size(); buf.clear(); };
    for (uint32_t i : order) {
        buf.push_back(records[i]);
        if (buf.size() == buf.capacity()) flush();
    }
    flush();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) throw std::runtime_error("error writing " + path);
    std::fprintf(stderr, "wrote %s: %zu candidates, %.1f MB (%.1fs in total)\n", path.c_str(), sum.count,
                 (INDEX_HEADER_BYTES + 8.0 * sum.count) / 1e6, since(start));
    return sum;
}

IndexReader::IndexReader(const std::string& path, const Dictionary& dict) : dict_(dict)
{
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open " + path);
    std::string h(INDEX_HEADER_BYTES, '\0');
    if (std::fread(&h[0], 1, h.size(), f_) != h.size() || h.compare(0, 9, "VIGIDX 1\n") != 0)
        throw std::runtime_error(path + " is not a candidate index");
    std::istringstream in(h);
    std::string line, fingerprint;
    while (std::getline(in, line) && line != "end") {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if      (tag == "cipher") ls >> cipher_;
        else if (tag == "dict")   ls >> fingerprint;
        else if (tag == "count")  ls >> summary_.count;
        else if (tag == "kl")     { int kl; size_t c; ls >> kl >> c; summary_.per_key_length.push_back({kl, c}); }
        else if (tag == "note")   summary_.notes.push_back(line.substr(5));
    }
    if (fingerprint != hex(dict.fingerprint))
        throw std::runtime_error(path + " was built with a different dictionary");
}

IndexReader::~IndexReader() { if (f_) std::fclose(f_); }

Candidate IndexReader::decode(size_t rank, uint64_t record) const
{
    Candidate c;
    const int kl = record_kl(record);
    c.rank  = rank;
    c.key   = unpack_key(record_code(record), kl);
    c.plain = vig_decrypt(cipher_, c.key);
    // Same arithmetic as candidate_score(), so scores match the stored order exactly.
    c.score = segment(dict_, c.plain, &c.words) - kl * std::log((double)ALPHA);
    return c;
}

std::vector<Candidate> IndexReader::range(size_t first, size_t last)
{
    std::vector<Candidate> out;
    first = std::max<size_t>(first, 1);
    last  = std::min(last, summary_.count);
    if (first > last) return out;
    std::fseek(f_, (long)(INDEX_HEADER_BYTES + 8 * (first - 1)), SEEK_SET);
    for (size_t r = first; r <= last; r++) {
        uint64_t rec;
        if (std::fread(&rec, 8, 1, f_) != 1) break;
        out.push_back(decode(r, rec));
    }
    return out;
}

size_t IndexReader::rank_of(const std::string& plain)
{
    if (plain.size() != cipher_.size()) return 0;
    std::fseek(f_, (long)INDEX_HEADER_BYTES, SEEK_SET);
    std::vector<uint64_t> buf(1 << 16);
    size_t r = 0, got;
    while ((got = std::fread(buf.data(), 8, buf.size(), f_)) > 0)
        for (size_t i = 0; i < got; i++) {
            ++r;
            const int kl = record_kl(buf[i]);
            if (vig_decrypt(cipher_, unpack_key(record_code(buf[i]), kl)) == plain) return r;
        }
    return 0;
}
