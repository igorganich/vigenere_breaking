#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static constexpr int ALPHA = 26;

// Letter trie with 26-way child arrays.
struct Trie {
    std::vector<std::array<int, ALPHA>> ch;     // -1 = no child
    std::vector<char>                   mark;   // a string inserted into the trie ends here
    std::vector<int>                    parent;
    std::vector<char>                   letter;

    Trie() { add_node(-1, 0); }

    int add_node(int par, char c) {
        std::array<int, ALPHA> a;
        a.fill(-1);
        ch.push_back(a); mark.push_back(0); parent.push_back(par); letter.push_back(c);
        return (int)ch.size() - 1;
    }
    void insert(const std::string& w) {
        int u = 0;
        for (char c : w) {
            int x = c - 'a';
            if (ch[u][x] < 0) { int v = add_node(u, c); ch[u][x] = v; }
            u = ch[u][x];
        }
        mark[u] = 1;
    }
    int find(const std::string& w) const {
        int u = 0;
        for (char c : w) if ((u = ch[u][c - 'a']) < 0) return -1;
        return u;
    }
    std::string str(int u) const {   // the string spelled from the root to node u
        std::string s;
        for (; u > 0; u = parent[u]) s += letter[u];
        return {s.rbegin(), s.rend()};
    }
};

// Word list with frequency ranks and the two tries the search walks.
struct Dictionary {
    std::unordered_set<std::string>     words;     // 2+ letters from the list, plus "a" and "i"
    std::unordered_map<std::string,int> rank;      // word -> 1-based frequency rank
    Trie                                trie;      // all words
    Trie                                suffixes;  // all proper word suffixes (word[i:], i >= 1)
    std::vector<double>                 node_logp; // trie node -> word log-probability (word ends)
    std::vector<int>                    node_rank; // trie node -> word rank (word ends)
    uint64_t                            fingerprint = 1469598103934665603ull;  // FNV-1a of the file
};

// Word probabilities follow Zipf's law over the frequency-ordered list:
// P(rank r) = (1/r) / Σ 1/r'.
inline Dictionary load_dictionary(const std::string& path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    Dictionary d;
    int r = 1;
    std::string w;
    while (f >> w) {
        for (char c : w) { d.fingerprint ^= (unsigned char)c; d.fingerprint *= 1099511628211ull; }
        d.fingerprint ^= '\n'; d.fingerprint *= 1099511628211ull;
        if (w.size() >= 2) { d.words.insert(w); d.rank[w] = r++; }
    }
    // The list has no 1-letter words; add "a" and "i" at their approximate English ranks.
    for (auto [word, rk] : {std::pair<const char*, int>{"a", 5}, {"i", 11}})
        if (d.words.insert(word).second) d.rank[word] = rk;

    for (const auto& x : d.words) {
        d.trie.insert(x);
        for (size_t i = 1; i < x.size(); i++) d.suffixes.insert(x.substr(i));
    }
    double h = 0;
    for (int k = 1; k <= (int)d.rank.size(); k++) h += 1.0 / k;
    const double logz = std::log(h);
    d.node_logp.assign(d.trie.ch.size(), 0);
    d.node_rank.assign(d.trie.ch.size(), 0);
    for (const auto& [word, rk] : d.rank) {
        int u = d.trie.find(word);
        d.node_rank[u] = rk;
        d.node_logp[u] = -std::log((double)rk) - logz;
    }
    return d;
}
