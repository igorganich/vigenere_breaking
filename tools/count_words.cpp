// Count the words of a text corpus, for building a word list in a new language.
//
//   count_words MIN_COUNT < corpus.txt > counts.txt
//
// The corpus is read line by line (one sentence or subtitle line per line, UTF-8).  A word
// is a run of letters; apostrophes are dropped ("don't" -> "dont") and anything else
// separates words.  Words are reduced to the cipher alphabet a-z: letters are lowercased
// and German umlauts and ß are transliterated as ciphers write them (ä -> ae, ö -> oe,
// ü -> ue, ß -> ss).  A word with any other letter (é, ñ, Cyrillic, ...) is skipped.
//
// Output, most frequent first, for words seen at least MIN_COUNT times:
//   word count display
// where `display` is the word's most frequent spelling in the corpus not counting the first
// word of a line (whose capital letter says nothing): "schoen 12345 schön", "haus 999 Haus".
//
// Build: g++ -O2 -std=c++17 -o count_words tools/count_words.cpp

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Entry {
    uint64_t count = 0;
    std::unordered_map<std::string, uint64_t> forms;   // spelling -> count (not line-initial)
};

// Append the a-z form of one UTF-8 letter to `out`; false if it has none.
bool to_cipher(const std::string& s, size_t& i, std::string& out, std::string& orig)
{
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        if (!std::isalpha(c)) return false;
        out += (char)std::tolower(c); orig += (char)c; i++;
        return true;
    }
    if (c == 0xC3 && i + 1 < s.size()) {
        const unsigned char d = (unsigned char)s[i + 1];
        const char* t = nullptr;
        switch (d) {
            case 0xA4: case 0x84: t = "ae"; break;     // ä Ä
            case 0xB6: case 0x96: t = "oe"; break;     // ö Ö
            case 0xBC: case 0x9C: t = "ue"; break;     // ü Ü
            case 0x9F:            t = "ss"; break;     // ß
        }
        if (!t) return false;
        out += t; orig += s.substr(i, 2); i += 2;
        return true;
    }
    return false;
}

bool is_letter_start(const std::string& s, size_t i)
{
    const unsigned char c = (unsigned char)s[i];
    return c >= 0x80 || std::isalpha(c);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { std::cerr << "usage: count_words MIN_COUNT < corpus > counts\n"; return 1; }
    const uint64_t min_count = std::stoull(argv[1]);
    std::ios::sync_with_stdio(false);
    std::unordered_map<std::string, Entry> words;
    std::string line;
    uint64_t lines = 0, tokens = 0;
    while (std::getline(std::cin, line)) {
        bool first = true;
        for (size_t i = 0; i < line.size(); ) {
            if (!is_letter_start(line, i)) { i++; continue; }
            std::string w, orig;
            bool ok = true;
            while (i < line.size()) {
                if (line[i] == '\'') { i++; continue; }
                if (!is_letter_start(line, i)) break;
                const size_t before = i;
                if (!to_cipher(line, i, w, orig)) {
                    ok = false;
                    // skip the rest of this word: one UTF-8 sequence at a time
                    i = before + 1;
                    while (i < line.size() && ((unsigned char)line[i] & 0xC0) == 0x80) i++;
                }
            }
            if (ok && !w.empty()) {
                Entry& e = words[w];
                e.count++;
                if (!first) e.forms[orig]++;
                tokens++;
            }
            first = false;
        }
        if (++lines % 10'000'000 == 0) std::cerr << lines << " lines, " << tokens << " words\n";
    }
    std::vector<std::pair<std::string, const Entry*>> out;
    for (const auto& [w, e] : words)
        if (e.count >= min_count) out.push_back({w, &e});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){
        return a.second->count != b.second->count ? a.second->count > b.second->count : a.first < b.first;
    });
    for (const auto& [w, e] : out) {
        std::string display = w;
        uint64_t best = 0;
        for (const auto& [f, c] : e->forms)
            if (c > best || (c == best && f < display)) { best = c; display = f; }
        std::cout << w << ' ' << e->count << ' ' << display << '\n';
    }
    std::cerr << lines << " lines, " << tokens << " words, " << words.size() << " distinct, "
              << out.size() << " seen at least " << min_count << " times\n";
}
