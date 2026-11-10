#pragma once
#include "tables.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ── Language models for ranking ───────────────────────────────────────────────
//
//   unigram  P(w) = c(w) / Σ c over the dictionary, from txt/<lang>/unigrams.txt.
//   bigram   P(w₂ | w₁) from txt/<lang>/bigrams.txt, backing off to the unigram model:
//              c(w₁ w₂) / c(w₁)       for pairs in the table,
//              α(w₁) · P(w₂)          for the rest,
//            where α(w₁) spreads the probability the listed pairs leave over the words
//            not listed after w₁, so P(· | w₁) sums to 1.  The table only holds pairs
//            seen at least 100,000 times, so every unlisted pair is rarer than that; no
//            parameter is tuned.
//   trigram  P(w₃ | w₁ w₂) from txt/<lang>/trigrams.txt, backing off to the bigram model:
//              (c(w₁ w₂ w₃) − D) / c(w₁ w₂)   for trigrams in the table,
//              α(w₁ w₂) · P(w₃ | w₂)           for the rest,
//            with c(w₁ w₂) from txt/<lang>/trigram_contexts.txt (every continuation,
//            including trigrams below the table's cutoff) and α(w₁ w₂) again making
//            P(· | w₁ w₂) sum to 1.  D is the absolute discount n₁ / (n₁ + 2 n₂) stored
//            in the table.  A context without listed trigrams uses the bigram model.
// The first word of the text, and a word after a stray letter, use P(w); the second word
// uses P(w₂ | w₁).
//
// The unigram and bigram tables are made by tools/make_ngram_tables.py, the trigram tables
// by tools/count_trigrams.cpp.

enum class ModelKind { Unigram, Bigram, Trigram };

// Open-addressing hash map from a (trie node, trie node) pair to a log-probability.
class PairTable {
public:
    static uint64_t key(int u, int v) { return (uint64_t)(u + 1) << 32 | (uint32_t)v; }  // never 0
    static uint64_t key(int u, int v, int w) {        // trie nodes < 2^21; never 0
        return (uint64_t)(u + 1) << 42 | (uint64_t)v << 21 | (uint64_t)w;
    }

    void reserve(size_t n) {
        size_t cap = 16;
        while (cap < 2 * n) cap <<= 1;
        keys_.assign(cap, 0);
        vals_.assign(cap, 0);
        mask_ = cap - 1;
    }
    void put(uint64_t k, double v) {
        for (size_t i = hash(k) & mask_;; i = (i + 1) & mask_)
            if (keys_[i] == 0 || keys_[i] == k) { keys_[i] = k; vals_[i] = v; return; }
    }
    const double* get(uint64_t k) const {
        if (keys_.empty()) return nullptr;
        for (size_t i = hash(k) & mask_;; i = (i + 1) & mask_) {
            if (keys_[i] == k) return &vals_[i];
            if (keys_[i] == 0) return nullptr;
        }
    }

private:
    static uint64_t hash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; return k ^ (k >> 33); }
    std::vector<uint64_t> keys_;
    std::vector<double>   vals_;
    size_t                mask_ = 0;
};

struct LanguageModel {
    ModelKind           kind = ModelKind::Trigram;
    std::string         name = "trigram";
    std::string         fingerprint;        // FNV-1a of the table files
    double              stray_logp  = -40;  // a letter outside any word: below every word
                                            // (rarest word about -18, after a back-off about -23)
    std::vector<double> uni;                // trie node -> log P(w) (word ends)
    std::vector<double> backoff;            // trie node -> log α(w) (bigram)
    PairTable           pairs;              // (node w₁, node w₂) -> log P(w₂ | w₁) (bigram)
    PairTable           triples;            // (w₁, w₂, w₃) -> log P(w₃ | w₁ w₂) (trigram)
    PairTable           context;            // (w₁, w₂) -> log α(w₁ w₂) (trigram)

    // log P(v | u) under the bigram model.
    double bi(int u, int v) const {
        const double* p = pairs.get(PairTable::key(u, v));
        return p ? *p : backoff[u] + uni[v];
    }
    // log P(w | u v) under the trigram model.
    double tri(int u, int v, int w) const {
        if (const double* p = triples.get(PairTable::key(u, v, w))) return *p;
        const double* a = context.get(PairTable::key(u, v));
        return (a ? *a : 0) + bi(v, w);
    }
};

inline const char* MODEL_NAMES = "unigram, bigram, trigram";

inline bool parse_model_kind(const std::string& s, ModelKind* k)
{
    if (s == "unigram") { *k = ModelKind::Unigram; return true; }
    if (s == "bigram")  { *k = ModelKind::Bigram;  return true; }
    if (s == "trigram") { *k = ModelKind::Trigram; return true; }
    return false;
}

namespace model_detail {

inline std::string read_all(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

inline void fnv(uint64_t& h, const std::string& s)
{
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
}

} // namespace model_detail

// `txt_dir` (txt/<lang>) holds the *grams*.txt tables.  Throws std::runtime_error.
inline LanguageModel load_model(ModelKind kind, const Dictionary& d, const std::string& txt_dir)
{
    using namespace model_detail;
    LanguageModel m;
    m.kind = kind;
    m.name = kind == ModelKind::Unigram ? "unigram" : kind == ModelKind::Bigram ? "bigram" : "trigram";
    uint64_t fp = 1469598103934665603ull;
    // A word of a table (UTF-8 in the dictionary's alphabet) -> its trie node, or -1.
    std::string scratch;
    auto node = [&](const std::string& w) {
        if (!d.alphabet.encode_word(w, &scratch)) return -1;
        int u = d.trie.find(scratch);
        return u >= 0 && d.trie.mark[u] ? u : -1;
    };

    // Unigram counts.
    const std::string uni_path = txt_dir + "/unigrams.txt";
    const std::string uni_text = read_all(uni_path);
    fnv(fp, uni_text);
    std::vector<double> count(d.trie.ch.size(), 0);
    {
        std::istringstream in(uni_text);
        std::string w;
        double c;
        while (in >> w >> c) {
            int u = node(w);
            if (u >= 0) count[u] = c;
        }
    }
    // Custom words (Dictionary::extra) that the table lacks get its smallest count, the count
    // the table already gives words never seen in the corpus.
    double floor = 0;
    for (double c : count) if (c > 0 && (floor == 0 || c < floor)) floor = c;
    for (const auto& w : d.extra) {
        int u = d.trie.find(w);
        if (count[u] <= 0) count[u] = floor;
    }
    double total = 0;
    for (const auto& w : d.words) {
        int u = d.trie.find(w);
        if (count[u] <= 0) throw std::runtime_error(uni_path + " has no count for \"" + w +
                                                    "\"; rebuild it with tools/make_ngram_tables.py");
        total += count[u];
    }
    m.uni.assign(d.trie.ch.size(), 0);
    for (const auto& w : d.words) {
        int u = d.trie.find(w);
        m.uni[u] = std::log(count[u] / total);
    }

    if (kind == ModelKind::Bigram || kind == ModelKind::Trigram) {
        const std::string bi_path = txt_dir + "/bigrams.txt";
        const std::string bi_text = read_all(bi_path);
        fnv(fp, bi_text);
        struct Pair { int u, v; double c; };
        std::vector<Pair> list;
        {
            std::istringstream in(bi_text);
            std::string a, b;
            double c;
            while (in >> a >> b >> c) {
                int u = node(a), v = node(b);
                if (u >= 0 && v >= 0) list.push_back({u, v, c});
            }
        }
        // Per first word: the count the listed pairs take, and the unigram mass of the words
        // they lead to.  c(w₁) comes from a separate count list, so it is raised if needed
        // to leave at least 5% of the mass for unlisted pairs.
        std::vector<double> listed(d.trie.ch.size(), 0), listed_p(d.trie.ch.size(), 0);
        for (const auto& p : list) { listed[p.u] += p.c; listed_p[p.u] += std::exp(m.uni[p.v]); }
        std::vector<double> c1(d.trie.ch.size(), 0);
        for (size_t u = 0; u < c1.size(); u++) c1[u] = std::max(count[u], listed[u] / 0.95);
        m.pairs.reserve(list.size());
        for (const auto& p : list) m.pairs.put(PairTable::key(p.u, p.v), std::log(p.c / c1[p.u]));
        m.backoff.assign(d.trie.ch.size(), 0);
        for (size_t u = 0; u < c1.size(); u++)
            if (listed[u] > 0) m.backoff[u] = std::log((1 - listed[u] / c1[u]) / (1 - listed_p[u]));
    }
    if (kind == ModelKind::Trigram) {
        if (d.trie.ch.size() >= (1u << 21)) throw std::runtime_error("trie too large for trigram keys");
        // Context counts c(w₁ w₂).
        const std::string ctx_path = txt_dir + "/trigram_contexts.txt";
        const std::string ctx_text = read_all(ctx_path);
        fnv(fp, ctx_text);
        PairTable ctx_count;
        size_t n_ctx = 0;
        for (char c : ctx_text) n_ctx += c == '\n';
        ctx_count.reserve(n_ctx);
        {
            std::istringstream in(ctx_text);
            std::string a, b;
            double c;
            while (in >> a >> b >> c) {
                int u = node(a), v = node(b);
                if (u >= 0 && v >= 0) ctx_count.put(PairTable::key(u, v), c);
            }
        }
        // Trigram counts; the first line holds the discount.
        const std::string tri_path = txt_dir + "/trigrams.txt";
        const std::string tri_text = read_all(tri_path);
        fnv(fp, tri_text);
        std::istringstream in(tri_text);
        std::string hash_sign, word;
        double D = -1;
        if (!(in >> hash_sign >> word >> D) || hash_sign != "#" || word != "discount" || D < 0 || D >= 1)
            throw std::runtime_error(tri_path + ": first line must be \"# discount D\"");
        struct Tri { int u, v, w; double c; };
        std::vector<Tri> list;
        {
            std::string a, b, c;
            double n;
            while (in >> a >> b >> c >> n) {
                int u = node(a), v = node(b), w = node(c);
                if (u >= 0 && v >= 0 && w >= 0) list.push_back({u, v, w, n});
            }
        }
        // Per context: the discounted mass of the listed trigrams, and the bigram mass of
        // the words they lead to; α(w₁ w₂) = (1 − listed) / (1 − listed_bigram).
        PairTable listed, listed_bi;
        listed.reserve(n_ctx);
        listed_bi.reserve(n_ctx);
        auto add = [](PairTable& t, uint64_t k, double x) {
            const double* p = t.get(k);
            t.put(k, (p ? *p : 0) + x);
        };
        m.triples.reserve(list.size());
        for (const auto& t : list) {
            const uint64_t k = PairTable::key(t.u, t.v);
            const double* c12 = ctx_count.get(k);
            if (!c12 || *c12 < t.c) throw std::runtime_error(tri_path + ": context count missing or too small");
            const double p = (t.c - D) / *c12;
            m.triples.put(PairTable::key(t.u, t.v, t.w), std::log(p));
            add(listed, k, p);
            add(listed_bi, k, std::exp(m.bi(t.v, t.w)));
        }
        m.context.reserve(n_ctx);
        for (const auto& t : list) {
            const uint64_t k = PairTable::key(t.u, t.v);
            if (m.context.get(k)) continue;
            m.context.put(k, std::log((1 - *listed.get(k)) / std::max(1e-12, 1 - *listed_bi.get(k))));
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)fp);
    m.fingerprint = buf;
    return m;
}
