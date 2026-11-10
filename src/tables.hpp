#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ── Letters ───────────────────────────────────────────────────────────────────
//
// Text is handled internally as one byte per letter: letter i of the cipher alphabet is the
// byte 'a' + i (so for a-z the internal form is the text itself; the 33 Ukrainian letters
// take bytes up to 'a' + 32).  Alphabet converts between UTF-8 text and that form.

static constexpr int MAX_ALPHA = 33;     // the largest alphabet (Ukrainian)

inline int  letter_index(char c) { return (unsigned char)c - 'a'; }
inline char letter_char(int i)   { return (char)('a' + i); }

struct Alphabet {
    std::vector<std::string> lower, upper;       // letter i in UTF-8, both cases

    static Alphabet latin() {
        Alphabet a;
        for (char c = 'a'; c <= 'z'; c++) { a.lower.push_back(std::string(1, c)); a.upper.push_back(std::string(1, (char)(c - 32))); }
        a.build();
        return a;
    }
    // From the lowercase and uppercase letters, in alphabet order, as UTF-8 strings.
    static Alphabet of(const std::string& lower_letters, const std::string& upper_letters) {
        Alphabet a;
        for (size_t i = 0; i < lower_letters.size(); i += utf8_len(lower_letters[i])) a.lower.push_back(lower_letters.substr(i, utf8_len(lower_letters[i])));
        for (size_t i = 0; i < upper_letters.size(); i += utf8_len(upper_letters[i])) a.upper.push_back(upper_letters.substr(i, utf8_len(upper_letters[i])));
        if (a.lower.size() != a.upper.size() || a.lower.size() > (size_t)MAX_ALPHA) throw std::runtime_error("bad alphabet");
        a.build();
        return a;
    }
    int size() const { return (int)lower.size(); }

    // The index of the letter at text[i] (either case) and its byte length, or -1.
    int letter_at(const std::string& text, size_t i, size_t* len) const {
        const uint32_t cp = code_point(text, i, len);
        if (cp < 128) return ascii_[cp];
        auto it = other_.find(cp);
        return it == other_.end() ? -1 : it->second;
    }
    // The letters of `text` in internal form; everything else is dropped.
    std::string letters_of(const std::string& text) const {
        std::string out;
        size_t len;
        for (size_t i = 0; i < text.size(); i += len) {
            const int k = letter_at(text, i, &len);
            if (k >= 0) out += letter_char(k);
        }
        return out;
    }
    // A whole word in internal form, apostrophes dropped ("don't", "м'ясо"); false if it has
    // anything that is not a letter of this alphabet.
    bool encode_word(const std::string& w, std::string* out) const {
        out->clear();
        size_t len;
        for (size_t i = 0; i < w.size(); i += len) {
            if (is_apostrophe(w, i, &len)) continue;
            const int k = letter_at(w, i, &len);
            if (k < 0) return false;
            *out += letter_char(k);
        }
        return !out->empty();
    }
    // Internal form back to UTF-8 (lowercase); other bytes are copied.
    std::string decode(const std::string& internal) const {
        std::string out;
        for (char c : internal) {
            const int k = letter_index(c);
            if (k >= 0 && k < size()) out += lower[k]; else out += c;
        }
        return out;
    }

    static uint32_t code_point(const std::string& s, size_t i, size_t* len) {
        const size_t n = std::min(utf8_len(s[i]), s.size() - i);
        *len = n;
        uint32_t cp = (unsigned char)s[i];
        if (n == 2) cp = (cp & 0x1F) << 6  | ((unsigned char)s[i + 1] & 0x3F);
        if (n == 3) cp = (cp & 0x0F) << 12 | ((unsigned char)s[i + 1] & 0x3F) << 6 | ((unsigned char)s[i + 2] & 0x3F);
        if (n == 4) cp = (cp & 0x07) << 18 | ((unsigned char)s[i + 1] & 0x3F) << 12 |
                         ((unsigned char)s[i + 2] & 0x3F) << 6 | ((unsigned char)s[i + 3] & 0x3F);
        return cp;
    }
    static size_t utf8_len(char c) {
        const unsigned char u = (unsigned char)c;
        return u < 0x80 ? 1 : u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
    }
    static bool is_apostrophe(const std::string& w, size_t i, size_t* len) {
        for (const char* a : {"'", "\xE2\x80\x99", "\xCA\xBC"})        // ' ’ ʼ
            if (w.compare(i, std::strlen(a), a) == 0) { *len = std::strlen(a); return true; }
        return false;
    }

private:
    void build() {
        ascii_.fill(-1);
        for (int k = 0; k < size(); k++)
            for (const auto* v : {&lower[k], &upper[k]}) {
                size_t len;
                const uint32_t cp = code_point(*v, 0, &len);
                if (cp < 128) ascii_[cp] = (int8_t)k; else other_[cp] = k;
            }
    }
    std::array<int8_t, 128>           ascii_{};
    std::unordered_map<uint32_t, int> other_;
};

// Letter trie with child arrays for the largest alphabet.
struct Trie {
    std::vector<std::array<int, MAX_ALPHA>> ch;     // -1 = no child
    std::vector<char>                   mark;   // a string inserted into the trie ends here
    std::vector<int>                    parent;
    std::vector<char>                   letter;

    Trie() { add_node(-1, 0); }

    int add_node(int par, char c) {
        std::array<int, MAX_ALPHA> a;
        a.fill(-1);
        ch.push_back(a); mark.push_back(0); parent.push_back(par); letter.push_back(c);
        return (int)ch.size() - 1;
    }
    void insert(const std::string& w) {
        int u = 0;
        for (char c : w) {
            int x = letter_index(c);
            if (ch[u][x] < 0) { int v = add_node(u, c); ch[u][x] = v; }
            u = ch[u][x];
        }
        mark[u] = 1;
    }
    int find(const std::string& w) const {
        int u = 0;
        for (char c : w) if ((u = ch[u][letter_index(c)]) < 0) return -1;
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
    std::unordered_set<std::string>     words;     // 2+ letters from the list, plus the language's 1-letter words
    std::unordered_map<std::string,int> rank;      // word -> 1-based frequency rank
    Trie                                trie;      // all words
    Trie                                suffixes;  // all proper word suffixes (word[i:], i >= 1)
    std::vector<int>                    node_rank; // trie node -> word rank (word ends)
    std::vector<std::string>            extra;     // words added from a custom file, in its order
    std::unordered_map<std::string,std::string> display;  // word -> spelling for GPT-2 (load_display)
    std::string                         lang = "en";      // language code (lang.hpp), recorded in indexes
    Alphabet                            alphabet = Alphabet::latin();   // words are kept in its internal form
    uint64_t                            fingerprint = 1469598103934665603ull;  // FNV-1a of the file(s)
};

// Words of a custom word file: one or more per line, '#' starts a comment; letters are
// lowercased and anything else separates words.  Single letters are skipped (1-letter words
// are fixed per language).  Throws std::runtime_error if the file cannot be read.
// The words come back in the internal form of `alphabet`; apostrophes are dropped.
inline std::vector<std::string> read_word_file(const std::string& path, const Alphabet& alphabet = Alphabet::latin())
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        std::string w;
        size_t len;
        for (size_t i = 0; i <= line.size(); i += len) {
            len = 1;
            if (i < line.size() && Alphabet::is_apostrophe(line, i, &len)) continue;
            const int k = i < line.size() ? alphabet.letter_at(line, i, &len) : -1;
            if (k >= 0) { w += letter_char(k); continue; }
            if (w.size() >= 2) out.push_back(w);
            w.clear();
        }
    }
    return out;
}

// Ranks are only shown next to words; probabilities come from the language model (model.hpp).
// `extra` adds custom words (read_word_file) after the list, ranked after its last word; they
// change the fingerprint, so an index built with them is only read with the same words.
// `singles` are the language's 1-letter words with their approximate ranks (English "a" and
// "i"; lang.hpp), which the list itself leaves out.  The list is UTF-8 in `alphabet`; its
// words, `extra` and `singles` (UTF-8) are kept in the alphabet's internal form.
inline Dictionary load_dictionary(const std::string& path, const std::vector<std::string>& extra = {},
                                  const std::vector<std::pair<std::string, int>>& singles = {{"a", 5}, {"i", 11}},
                                  const Alphabet& alphabet = Alphabet::latin())
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    Dictionary d;
    d.alphabet = alphabet;
    int r = 1;
    std::string raw, w;
    while (f >> raw) {
        for (char c : raw) { d.fingerprint ^= (unsigned char)c; d.fingerprint *= 1099511628211ull; }
        d.fingerprint ^= '\n'; d.fingerprint *= 1099511628211ull;
        if (alphabet.encode_word(raw, &w) && w.size() >= 2 && !d.words.count(w)) { d.words.insert(w); d.rank[w] = r++; }
    }
    bool marked = false;
    for (const auto& x : extra) {
        if (d.words.count(x)) continue;
        if (!marked) { d.fingerprint ^= '+'; d.fingerprint *= 1099511628211ull; marked = true; }
        for (char c : x) { d.fingerprint ^= (unsigned char)c; d.fingerprint *= 1099511628211ull; }
        d.fingerprint ^= '\n'; d.fingerprint *= 1099511628211ull;
        d.words.insert(x); d.rank[x] = r++; d.extra.push_back(x);
    }
    for (const auto& [single, rk] : singles)
        if (alphabet.encode_word(single, &w) && d.words.insert(w).second) d.rank[w] = rk;

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

// Spellings for GPT-2 from a "word Spelling" file (lang.hpp); a missing file leaves none.
inline void load_display(Dictionary& d, const std::string& path)
{
    std::ifstream f(path);
    std::string raw, w, spelling;
    while (f >> raw >> spelling)
        if (d.alphabet.encode_word(raw, &w) && d.words.count(w)) d.display[w] = spelling;
}
