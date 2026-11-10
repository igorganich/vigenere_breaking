// Count the words of a text corpus, for building a word list in a new language.
//
//   count_words MIN_COUNT < corpus.txt > counts.txt
//
// The corpus is read line by line (one sentence or subtitle line per line, UTF-8).  A word
// is a run of letters and apostrophes, normalized as tools/normalize.hpp describes: Latin
// letters lowercased with German umlauts and ß transliterated (ä -> ae ... ß -> ss),
// Ukrainian letters lowercased in UTF-8, apostrophes dropped ("don't" -> "dont",
// "м'ясо" -> "мясо").  A word with any other letter (é, ñ, Russian ы, ...) is skipped.
//
// Output, most frequent first, for words seen at least MIN_COUNT times:
//   word count display
// where `display` is the word's most frequent spelling in the corpus not counting the first
// word of a line (whose capital letter says nothing): "schoen 12345 schön", "haus 999 Haus",
// "мясо 3105 м'ясо".
//
// Build: g++ -O2 -std=c++17 -o count_words tools/count_words.cpp

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "normalize.hpp"

namespace {

struct Entry {
    uint64_t count = 0;
    std::unordered_map<std::string, uint64_t> forms;   // spelling -> count (not line-initial)
};

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
            std::string w, orig;
            bool foreign = false, any = false;
            for (;;) {                                   // one word: letters and apostrophes
                if (i >= line.size()) break;
                const size_t before = i;
                const norm::Kind k = norm::next(line, i, w, orig);
                if (k == norm::Separator) { if (!any) continue; i = before + 1; break; }
                if (k == norm::Foreign) foreign = true;
                any = true;
            }
            if (!any) continue;
            while (!orig.empty() && orig.back() == '\'') orig.pop_back();       // quotes around a word
            while (!orig.empty() && orig.front() == '\'') orig.erase(0, 1);
            if (!foreign && !w.empty()) {
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
