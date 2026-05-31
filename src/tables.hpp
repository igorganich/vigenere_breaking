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
    std::vector<std::string>         dict;       // ≥ 3 chars — key derivation
    std::vector<std::string>         dict_full;  // ≥ 2 chars — coverage validation
    std::unordered_set<std::string>  dict_set;   // dict_full as hash set — O(1) word lookup
    std::unordered_map<std::string,int> word_rank; // word → 1-based rank (lower = more common)
    int                              max_word_len = 0;
};

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
            if (w.size() >= 3) t.dict.push_back(w);
            t.max_word_len = std::max(t.max_word_len, (int)w.size());
        }
    }

    return t;
}
