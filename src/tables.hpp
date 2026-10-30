#pragma once
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

static constexpr int  ALPHA    = 26;
static constexpr int  QUAD_LEN = ALPHA * ALPHA * ALPHA * ALPHA; // 456 976
static constexpr double NO_LOG = -15.0;

struct Tables {
    std::array<double, QUAD_LEN> quad{};
    std::vector<std::string>         dict_full;  // ≥ 2 chars in frequency order, then "a", "i"
    std::unordered_set<std::string>  dict_set;   // dict_full as hash set — O(1) word lookup
    std::unordered_map<std::string,int> word_rank; // word → 1-based rank (lower = more common)
    int                              max_word_len = 0;
    double                           log_zipf_norm = 0.0; // ln Σ 1/r over all ranks
};

// Unigram word log-probability estimated from frequency rank (Zipf's law):
// P(rank r) = (1/r) / Σ 1/r'.
inline double word_logp(int rank, const Tables& t)
{
    return -std::log((double)rank) - t.log_zipf_norm;
}

inline Tables load_tables(const char* quad_file, const char* dict_file)
{
    Tables t;
    t.quad.fill(NO_LOG);

    // Quadgrams: "tion 13168375"  (lowercase, 4-letter key + count)
    {
        std::ifstream f(quad_file);
        if (!f) throw std::runtime_error(std::string("cannot open ") + quad_file);
        std::string ng;
        long cnt, total = 0;
        std::vector<std::pair<int, long>> rows;
        while (f >> ng >> cnt) {
            if ((int)ng.size() != 4) continue;
            int idx = (ng[0]-'a') * 17576
                    + (ng[1]-'a') *   676
                    + (ng[2]-'a') *    26
                    + (ng[3]-'a');
            rows.push_back({idx, cnt});
            total += cnt;
        }
        double inv = 1.0 / (double)total;
        for (auto [idx, c] : rows)
            t.quad[idx] = std::log((double)c * inv);
    }

    {
        std::ifstream f(dict_file);
        if (!f) throw std::runtime_error(std::string("cannot open ") + dict_file);
        std::string w;
        int rank = 1;
        while (f >> w) {
            if (w.size() >= 2) {
                t.dict_full.push_back(w);
                t.dict_set.insert(w);
                t.word_rank[w] = rank++;
            }
            t.max_word_len = std::max(t.max_word_len, (int)w.size());
        }
        // The word list has no 1-letter words; add "a" and "i" with their
        // approximate English frequency ranks (ranks of other words unchanged).
        for (auto [w, rank] : {std::pair<const char*, int>{"a", 5}, {"i", 11}}) {
            if (t.dict_set.insert(w).second) {
                t.dict_full.push_back(w);
                t.word_rank[w] = rank;
            }
        }
        double h = 0.0;
        for (int r = 1; r <= (int)t.word_rank.size(); r++) h += 1.0 / r;
        t.log_zipf_norm = std::log(h);
    }

    return t;
}
