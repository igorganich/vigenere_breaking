#pragma once
#include <array>
#include <cctype>
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
    std::vector<int>                    node_rank; // trie node -> word rank (word ends)
    std::vector<std::string>            extra;     // words added from a custom file, in its order
    uint64_t                            fingerprint = 1469598103934665603ull;  // FNV-1a of the file(s)
};

// Words of a custom word file: one or more per line, '#' starts a comment; letters are
// lowercased and anything else separates words.  Single letters are skipped (the dictionary
// has only "a" and "i").  Throws std::runtime_error if the file cannot be read.
inline std::vector<std::string> read_word_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        std::string w;
        for (size_t i = 0; i <= line.size(); i++) {
            const char c = i < line.size() ? line[i] : ' ';
            if (std::isalpha((unsigned char)c)) { w += (char)std::tolower((unsigned char)c); continue; }
            if (w.size() >= 2) out.push_back(w);
            w.clear();
        }
    }
    return out;
}

// Ranks are only shown next to words; probabilities come from the language model (model.hpp).
// `extra` adds custom words (read_word_file) after the list, ranked after its last word; they
// change the fingerprint, so an index built with them is only read with the same words.
inline Dictionary load_dictionary(const std::string& path, const std::vector<std::string>& extra = {})
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
    bool marked = false;
    for (const auto& x : extra) {
        if (d.words.count(x)) continue;
        if (!marked) { d.fingerprint ^= '+'; d.fingerprint *= 1099511628211ull; marked = true; }
        for (char c : x) { d.fingerprint ^= (unsigned char)c; d.fingerprint *= 1099511628211ull; }
        d.fingerprint ^= '\n'; d.fingerprint *= 1099511628211ull;
        d.words.insert(x); d.rank[x] = r++; d.extra.push_back(x);
    }
    // The list has no 1-letter words; add "a" and "i" at their approximate English ranks.
    for (auto [word, rk] : {std::pair<const char*, int>{"a", 5}, {"i", 11}})
        if (d.words.insert(word).second) d.rank[word] = rk;

    for (const auto& x : d.words) {
        d.trie.insert(x);
        for (size_t i = 1; i < x.size(); i++) d.suffixes.insert(x.substr(i));
    }
    d.node_rank.assign(d.trie.ch.size(), 0);
    for (const auto& [word, rk] : d.rank) {
        int u = d.trie.find(word);
        d.node_rank[u] = rk;
    }
    return d;
}
