#include "index.hpp"
#include "cipher.hpp"
#include "dp.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {

// A key as one 8-byte record.  With 26 letters: the key length in the top 6 bits and the key
// packed base 26 below.  With a larger alphabet of A letters (33¹² does not fit in 58 bits):
// the key in bijective base A + 1, Σ (letter(key[i]) + 1) · (A + 1)^i, whose digit count is
// the key length (34¹² < 2⁶³).
inline uint64_t make_record(const std::string& key, int A)
{
    if (A == 26) return (uint64_t)key.size() << 58 | pack_key(key, A);
    uint64_t r = 0, p = 1;
    for (char c : key) { r += (uint64_t)(letter_index(c) + 1) * p; p *= (uint64_t)(A + 1); }
    return r;
}
inline std::string record_key(uint64_t r, int A)
{
    if (A == 26) return unpack_key(r & ((1ull << 58) - 1), (int)(r >> 58), A);
    std::string key;
    for (; r; r /= (uint64_t)(A + 1)) key += letter_char((int)(r % (uint64_t)(A + 1)) - 1);
    return key;
}

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

struct Header {
    std::string  cipher, lang = "en", dict, model = "zipf", model_fp;
    int          max_key_length = 12;
    IndexSummary sum;
};

Header read_header(FILE* f, const std::string& path)
{
    std::string h(INDEX_HEADER_BYTES, '\0');
    if (std::fread(&h[0], 1, h.size(), f) != h.size() || h.compare(0, 9, "VIGIDX 1\n") != 0)
        throw std::runtime_error(path + " is not a candidate index");
    Header out;
    std::istringstream in(h);
    std::string line;
    while (std::getline(in, line) && line != "end") {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if      (tag == "cipher")         ls >> out.cipher;
        else if (tag == "lang")           ls >> out.lang;
        else if (tag == "dict")           ls >> out.dict;
        else if (tag == "model")          ls >> out.model >> out.model_fp;
        else if (tag == "max_key_length") ls >> out.max_key_length;
        else if (tag == "count")          ls >> out.sum.count;
        else if (tag == "kl")             { int kl; size_t c; ls >> kl >> c; out.sum.per_key_length.push_back({kl, c}); }
        else if (tag == "note")           out.sum.notes.push_back(line.substr(5));
    }
    return out;
}

// Temporary files, removed when the object goes out of scope (also on errors).
class TempFiles {
public:
    explicit TempFiles(std::string base) : base_(std::move(base)) {}
    ~TempFiles() { for (const auto& p : paths_) std::remove(p.c_str()); }
    std::string make(const std::string& suffix) { paths_.push_back(base_ + suffix); return paths_.back(); }
    void forget(const std::string& p) {                 // already removed or renamed
        paths_.erase(std::remove(paths_.begin(), paths_.end(), p), paths_.end());
    }
private:
    std::string              base_;
    std::vector<std::string> paths_;
};

FILE* open_or_throw(const std::string& path, const char* mode)
{
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) throw std::runtime_error("cannot open " + path);
    return f;
}

void write_or_throw(const void* data, size_t size, size_t n, FILE* f, const std::string& path)
{
    if (n && std::fwrite(data, size, n, f) != n)
        throw std::runtime_error("error writing " + path + " (disk full?)");
}

void close_or_throw(FILE* f, const std::string& path)
{
    if (std::fclose(f) != 0) throw std::runtime_error("error writing " + path + " (disk full?)");
}

// A scored record; the index order is descending score, ties by record.
struct Entry {
    double   score;
    uint64_t record;
};
inline bool before(const Entry& a, const Entry& b)
{
    return a.score != b.score ? a.score > b.score : a.record < b.record;
}

// Sort with `threads` threads: sort equal slices in parallel, then merge them pairwise.
void parallel_sort(std::vector<Entry>& v, int threads)
{
    const size_t n = v.size(), parts = std::max(1, std::min(threads, (int)(n / 100000) + 1));
    std::vector<size_t> cut(parts + 1);
    for (size_t i = 0; i <= parts; i++) cut[i] = n * i / parts;
    parallel_for(parts, threads, [&](size_t i){ std::sort(v.begin() + cut[i], v.begin() + cut[i + 1], before); });
    for (size_t width = 1; width < parts; width *= 2) {
        const size_t pairs = (parts + 2 * width - 1) / (2 * width);
        parallel_for(pairs, threads, [&](size_t p){
            const size_t lo = 2 * width * p, mid = std::min(parts, lo + width), hi = std::min(parts, lo + 2 * width);
            if (mid < hi) std::inplace_merge(v.begin() + cut[lo], v.begin() + cut[mid], v.begin() + cut[hi], before);
        });
    }
}

// Score the `count` records stored at byte `offset` of `src` by `model` and write the index
// file, sorted by score (best first, ties by record).  Scores are computed in double exactly
// as the reader recomputes them, so the stored order is the order a reader sees.
//
// At most `chunk` records are held in memory (16 bytes each).  Larger inputs are sorted on
// disk: each chunk is scored, sorted and written as a run next to `path`, and the runs are
// merged into the index.  The result is the same file either way.
void write_ranked(const std::string& cipher, const Dictionary& dict, const LanguageModel& model,
                  int max_key_length, int threads, const IndexSummary& sum, const std::string& src,
                  long offset, size_t count, size_t chunk, const std::string& path)
{
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    chunk = std::max<size_t>(chunk, 1);
    TempFiles temp(path);
    const int A = dict.alphabet.size();

    // The header holds the ciphertext as text (UTF-8), not in internal form.
    std::string h = "VIGIDX 1\ncipher " + dict.alphabet.decode(cipher) + "\nlang " + dict.lang + "\ndict " + hex(dict.fingerprint) + " " +
                    std::to_string(dict.words.size()) + "\nmodel " + model.name + " " + model.fingerprint +
                    "\nmax_key_length " + std::to_string(max_key_length) +
                    "\ncount " + std::to_string(count) + "\n";
    for (auto [kl, c] : sum.per_key_length) h += "kl " + std::to_string(kl) + " " + std::to_string(c) + "\n";
    for (const auto& note : sum.notes) h += "note " + note + "\n";
    h += "end\n";
    if (h.size() > INDEX_HEADER_BYTES) throw std::runtime_error("index header too long");
    h.resize(INDEX_HEADER_BYTES, '\n');

    // Read, score and sort one chunk at a time.
    FILE* in = open_or_throw(src, "rb");
    std::fseek(in, offset, SEEK_SET);
    std::vector<uint64_t> records;
    std::vector<Entry> entries;
    std::vector<std::string> runs;
    size_t done = 0;
    try {
        do {
            const size_t m = std::min(chunk, count - done);
            records.resize(m);
            if (m && std::fread(records.data(), 8, m, in) != m) throw std::runtime_error(src + " is truncated");
            entries.resize(m);
            parallel_for(m, threads, [&](size_t i){
                const std::string key = record_key(records[i], A);
                entries[i] = {candidate_score(dict, model, vig_decrypt(cipher, key, A), (int)key.size()), records[i]};
            });
            parallel_sort(entries, threads);
            done += m;
            if (done == count && runs.empty()) break;      // everything fits in one chunk
            runs.push_back(temp.make(".run" + std::to_string(runs.size()) + ".tmp"));
            FILE* r = open_or_throw(runs.back(), "wb");
            try { write_or_throw(entries.data(), sizeof(Entry), entries.size(), r, runs.back()); }
            catch (...) { std::fclose(r); throw; }
            close_or_throw(r, runs.back());
        } while (done < count);
    } catch (...) { std::fclose(in); throw; }
    std::fclose(in);
    std::vector<uint64_t>().swap(records);

    // Write the index to a temporary file and rename, so rerank can overwrite its own input.
    const std::string tmp = temp.make(".tmp");
    FILE* f = open_or_throw(tmp, "wb");
    try {
        write_or_throw(h.data(), 1, h.size(), f, tmp);
        std::vector<uint64_t> buf;
        buf.reserve(1 << 16);
        auto put = [&](uint64_t rec) {
            buf.push_back(rec);
            if (buf.size() == buf.capacity()) { write_or_throw(buf.data(), 8, buf.size(), f, tmp); buf.clear(); }
        };
        if (runs.empty()) {
            for (const auto& e : entries) put(e.record);
        } else {
            std::vector<Entry>().swap(entries);
            // k-way merge of the sorted runs, each read through its own buffer.
            struct Run { FILE* f; std::vector<Entry> buf; size_t pos = 0; };
            std::vector<Run> rs(runs.size());
            const size_t per_run = std::max<size_t>(4096, (64u << 20) / sizeof(Entry) / runs.size());
            auto refill = [&](Run& r) {
                r.buf.resize(per_run);
                r.buf.resize(std::fread(r.buf.data(), sizeof(Entry), per_run, r.f));
                r.pos = 0;
                return !r.buf.empty();
            };
            auto by_entry = [&](size_t a, size_t b) { return before(rs[b].buf[rs[b].pos], rs[a].buf[rs[a].pos]); };
            std::vector<size_t> heap;
            try {
                for (size_t i = 0; i < runs.size(); i++) {
                    rs[i].f = open_or_throw(runs[i], "rb");
                    if (refill(rs[i])) heap.push_back(i);
                }
                std::make_heap(heap.begin(), heap.end(), by_entry);
                while (!heap.empty()) {
                    std::pop_heap(heap.begin(), heap.end(), by_entry);
                    Run& r = rs[heap.back()];
                    put(r.buf[r.pos].record);
                    if (++r.pos < r.buf.size() || refill(r)) std::push_heap(heap.begin(), heap.end(), by_entry);
                    else heap.pop_back();
                }
            } catch (...) {
                for (auto& r : rs) if (r.f) std::fclose(r.f);
                throw;
            }
            for (auto& r : rs) std::fclose(r.f);
        }
        write_or_throw(buf.data(), 8, buf.size(), f, tmp);
    } catch (...) { std::fclose(f); throw; }
    close_or_throw(f, tmp);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("cannot write " + path);
    temp.forget(tmp);
    std::fprintf(stderr, "ranked %zu candidates by %s (%.1fs%s)\n", count, model.name.c_str(),
                 std::chrono::duration<double>(Clock::now() - t0).count(),
                 runs.empty() ? "" : (", sorted on disk in " + std::to_string(runs.size()) + " runs").c_str());
}

} // namespace

IndexSummary build_index(const std::string& cipher, const Dictionary& dict, const LanguageModel& model,
                         const IndexOptions& opt, const std::string& path)
{
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    const auto start = Clock::now();
    const int  n = (int)cipher.size();
    const int  A = dict.alphabet.size();
    const int  threads = opt.threads > 0 ? opt.threads : std::max(1, (int)std::thread::hardware_concurrency());

    IndexSummary sum;
    // Candidates go to a temporary file as they are found: memory does not grow with them.
    TempFiles temp(path);
    const std::string keys_path = temp.make(".keys.tmp");
    FILE* keys = open_or_throw(keys_path, "w+b");
    size_t total = 0;
    try {
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
            const size_t room = opt.max_candidates - std::min(opt.max_candidates, total);
            std::mutex mu;
            size_t kept = 0;
            find_keys(cipher, kl, dict, threads, room, [&](const uint64_t* k, size_t m){
                std::vector<uint64_t> recs;
                recs.reserve(m);
                for (size_t i = 0; i < m; i++) {
                    const std::string key = unpack_key(k[i], kl, A);
                    if (expanded_period(key, n) == kl) recs.push_back(make_record(key, A));
                }
                std::lock_guard<std::mutex> lock(mu);
                write_or_throw(recs.data(), 8, recs.size(), keys, keys_path);
                kept += recs.size();
            }, &stats);
            if (stats.over_limit) {
                // Drop what this key length wrote before it went over the limit.
                if (std::fflush(keys) != 0 || ftruncate(fileno(keys), (off_t)(8 * total)) != 0)
                    throw std::runtime_error("error writing " + keys_path);
                std::fseek(keys, (long)(8 * total), SEEK_SET);
                sum.notes.push_back("key length " + std::to_string(kl) + " skipped: more than " +
                                    std::to_string(room) + " candidates (too ambiguous to list)");
                std::fprintf(stderr, "key length %2d: skipped, more than %zu candidates (%.1fs)\n", kl, room, since(t0));
                continue;
            }
            total += kept;
            sum.per_key_length.push_back({kl, kept});
            std::fprintf(stderr, "key length %2d: %zu candidates (%.1fs)\n", kl, kept, since(t0));
        }
    } catch (...) { std::fclose(keys); throw; }
    close_or_throw(keys, keys_path);
    sum.count = total;
    write_ranked(cipher, dict, model, opt.max_key_length, threads, sum, keys_path, 0, total, opt.sort_chunk, path);
    std::fprintf(stderr, "wrote %s: %zu candidates, %.1f MB (%.1fs in total)\n", path.c_str(), sum.count,
                 (INDEX_HEADER_BYTES + 8.0 * sum.count) / 1e6, since(start));
    return sum;
}

IndexSummary rerank_index(const std::string& in, const std::string& out, const Dictionary& dict,
                          const LanguageModel& model, int threads, size_t sort_chunk)
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    threads = threads > 0 ? threads : std::max(1, (int)std::thread::hardware_concurrency());
    Header h;
    {
        FILE* f = open_or_throw(in, "rb");
        try { h = read_header(f, in); } catch (...) { std::fclose(f); throw; }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        if (h.lang != dict.lang) throw std::runtime_error(in + " was built for language " + h.lang + ", not " + dict.lang);
        if (h.dict != hex(dict.fingerprint)) throw std::runtime_error(in + " was built with a different dictionary");
        if (size < (long)(INDEX_HEADER_BYTES + 8 * h.sum.count)) throw std::runtime_error(in + " is truncated");
    }
    write_ranked(dict.alphabet.letters_of(h.cipher), dict, model, h.max_key_length, threads, h.sum, in, (long)INDEX_HEADER_BYTES,
                 h.sum.count, sort_chunk, out);
    std::fprintf(stderr, "wrote %s: %zu candidates ranked by %s (%.1fs in total)\n", out.c_str(), h.sum.count,
                 model.name.c_str(), std::chrono::duration<double>(Clock::now() - start).count());
    return h.sum;
}

namespace {

Header header_of(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    Header h;
    try { h = read_header(f, path); } catch (...) { std::fclose(f); throw; }
    std::fclose(f);
    return h;
}

} // namespace

std::string index_model_name(const std::string& path) { return header_of(path).model; }
std::string index_lang(const std::string& path)       { return header_of(path).lang; }

IndexReader::IndexReader(const std::string& path, const Dictionary& dict, const LanguageModel& model)
    : dict_(dict), model_(model)
{
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open " + path);
    Header h;
    try { h = read_header(f_, path); } catch (...) { std::fclose(f_); f_ = nullptr; throw; }
    cipher_  = dict.alphabet.letters_of(h.cipher);
    summary_ = h.sum;
    std::string why;
    if (h.lang != dict.lang)
        why = path + " was built for language " + h.lang + ", not " + dict.lang;
    else if (h.dict != hex(dict.fingerprint))
        why = path + " was built with a different dictionary";
    else if (h.model == "zipf")
        why = path + " is ranked by the removed zipf model; update it with `rerank`";
    else if (h.model != model.name)
        why = path + " is ranked by the " + h.model + " model, not " + model.name;
    else if (h.model_fp != model.fingerprint)
        why = path + " was ranked with other " + h.model + " tables; rerank it";
    if (!why.empty()) { std::fclose(f_); f_ = nullptr; throw std::runtime_error(why); }
}

IndexReader::~IndexReader() { if (f_) std::fclose(f_); }

Candidate IndexReader::decode(size_t rank, uint64_t record) const
{
    Candidate c;
    const int A = dict_.alphabet.size();
    c.rank  = rank;
    c.key   = record_key(record, A);
    c.plain = vig_decrypt(cipher_, c.key, A);
    c.score = candidate_score(dict_, model_, c.plain, (int)c.key.size(), &c.words);   // as when ranked
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
            if (vig_decrypt(cipher_, record_key(buf[i], dict_.alphabet.size()), dict_.alphabet.size()) == plain) return r;
        }
    return 0;
}
