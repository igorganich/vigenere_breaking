#pragma once
#include "tables.hpp"
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
//   zipf     P(w) from the word's frequency rank (Zipf's law); the original model.
//   unigram  P(w) = c(w) / Σ c over the dictionary, from txt/english_unigrams.txt.
//   bigram   P(w₂ | w₁) from txt/english_bigrams.txt, backing off to the unigram model:
//              c(w₁ w₂) / c(w₁)       for pairs in the table,
//              α(w₁) · P(w₂)          for the rest,
//            where α(w₁) spreads the probability the listed pairs leave over the words
//            not listed after w₁, so P(· | w₁) sums to 1.  The table only holds pairs
//            seen at least 100,000 times, so every unlisted pair is rarer than that; no
//            parameter is tuned.  The first word of the text, and a word after a stray
//            letter, use P(w).
//
// The tables are made by tools/make_ngram_tables.py.

enum class ModelKind { Zipf, Unigram, Bigram };

// Open-addressing hash map from a (trie node, trie node) pair to a log-probability.
class PairTable {
public:
    static uint64_t key(int u, int v) { return (uint64_t)(u + 1) << 32 | (uint32_t)v; }  // never 0

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
    ModelKind           kind = ModelKind::Zipf;
    std::string         name = "zipf";
    std::string         fingerprint = "-";  // FNV-1a of the table files ("-" for zipf)
    double              stray_logp  = -15;  // a letter outside any word: below every word
    std::vector<double> uni;                // trie node -> log P(w) (word ends)
    std::vector<double> backoff;            // trie node -> log α(w) (bigram)
    PairTable           pairs;              // (node w₁, node w₂) -> log P(w₂ | w₁) (bigram)
};

inline const char* MODEL_NAMES = "zipf, unigram, bigram";

inline bool parse_model_kind(const std::string& s, ModelKind* k)
{
    if (s == "zipf")    { *k = ModelKind::Zipf;    return true; }
    if (s == "unigram") { *k = ModelKind::Unigram; return true; }
    if (s == "bigram")  { *k = ModelKind::Bigram;  return true; }
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

// `txt_dir` holds english_unigrams.txt and english_bigrams.txt.  Throws std::runtime_error.
inline LanguageModel load_model(ModelKind kind, const Dictionary& d, const std::string& txt_dir)
{
    using namespace model_detail;
    LanguageModel m;
    m.kind = kind;
    if (kind == ModelKind::Zipf) {
        m.uni = d.node_logp;
        return m;
    }
    m.name       = kind == ModelKind::Unigram ? "unigram" : "bigram";
    m.stray_logp = -40;     // rarest word: about -18; rarest word after a bigram back-off: about -23
    uint64_t fp  = 1469598103934665603ull;

    // Unigram counts.
    const std::string uni_path = txt_dir + "/english_unigrams.txt";
    const std::string uni_text = read_all(uni_path);
    fnv(fp, uni_text);
    std::vector<double> count(d.trie.ch.size(), 0);
    {
        std::istringstream in(uni_text);
        std::string w;
        double c;
        while (in >> w >> c) {
            int u = d.trie.find(w);
            if (u >= 0 && d.trie.mark[u]) count[u] = c;
        }
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

    if (kind == ModelKind::Bigram) {
        const std::string bi_path = txt_dir + "/english_bigrams.txt";
        const std::string bi_text = read_all(bi_path);
        fnv(fp, bi_text);
        struct Pair { int u, v; double c; };
        std::vector<Pair> list;
        {
            std::istringstream in(bi_text);
            std::string a, b;
            double c;
            while (in >> a >> b >> c) {
                int u = d.trie.find(a), v = d.trie.find(b);
                if (u >= 0 && v >= 0 && d.trie.mark[u] && d.trie.mark[v]) list.push_back({u, v, c});
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
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)fp);
    m.fingerprint = buf;
    return m;
}
