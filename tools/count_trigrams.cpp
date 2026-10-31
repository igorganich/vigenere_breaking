// Count word trigrams in a text corpus for the trigram ranking model.
//
//   count_trigrams WORDS MIN_COUNT TRIGRAMS_OUT CONTEXTS_OUT < corpus.txt
//
// The corpus is read line by line, one sentence (or subtitle line) per line.  Letters are
// lowercased, apostrophes dropped ("don't" -> "dont", as plaintexts are written), and any
// other character separates words.  Words are those of the WORDS list (2+ letters) plus "a"
// and "i".  Trigrams never cross a line or a word outside the list.
//
// TRIGRAMS_OUT: "w1 w2 w3 count" for trigrams seen at least MIN_COUNT times, sorted.
// CONTEXTS_OUT: "w1 w2 count" for every pair that starts a listed trigram, where count is
//               how often the pair is followed by another word of any kind on its line, so
//               the trigrams below MIN_COUNT and those into unlisted words are included.
// The first line of TRIGRAMS_OUT is "# discount D" with D = n1 / (n1 + 2 n2) over all
// trigrams (n_k: trigrams seen exactly k times), the absolute discount the model uses.
//
// Build: g++ -O2 -std=c++17 -o count_trigrams tools/count_trigrams.cpp

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 5) { std::cerr << "usage: count_trigrams WORDS MIN_COUNT TRIGRAMS_OUT CONTEXTS_OUT < corpus\n"; return 1; }
    std::vector<std::string> words;
    std::unordered_map<std::string, int> id;
    {
        std::ifstream f(argv[1]);
        if (!f) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }
        std::string w;
        while (f >> w)
            if (w.size() >= 2 && !id.count(w)) { id[w] = (int)words.size(); words.push_back(w); }
        for (const char* x : {"a", "i"})
            if (!id.count(x)) { id[x] = (int)words.size(); words.push_back(x); }
    }
    if (words.size() >= (1u << 21)) { std::cerr << "word list too long\n"; return 1; }
    const long min_count = std::stol(argv[2]);
    auto key3 = [](uint64_t a, uint64_t b, uint64_t c) { return a << 42 | b << 21 | c; };
    auto key2 = [](uint64_t a, uint64_t b) { return a << 21 | b; };

    std::unordered_map<uint64_t, uint32_t> tri, ctx;
    tri.reserve(1 << 26);
    ctx.reserve(1 << 24);
    std::string line, tok;
    std::vector<int> ids;      // word ids of the line, -1 = a word outside the list
    size_t lines = 0, tokens = 0;
    while (std::getline(std::cin, line)) {
        ids.clear();
        tok.clear();
        auto flush = [&]{
            if (tok.empty()) return;
            auto it = id.find(tok);
            ids.push_back(it == id.end() ? -1 : it->second);
            tok.clear();
        };
        for (unsigned char ch : line) {
            if (ch >= 'A' && ch <= 'Z') tok += (char)(ch - 'A' + 'a');
            else if (ch >= 'a' && ch <= 'z') tok += (char)ch;
            else if (ch == '\'') continue;
            else flush();
        }
        flush();
        tokens += ids.size();
        for (size_t i = 0; i + 2 < ids.size(); i++) {
            if (ids[i] < 0 || ids[i + 1] < 0) continue;
            ctx[key2(ids[i], ids[i + 1])]++;
            if (ids[i + 2] >= 0) tri[key3(ids[i], ids[i + 1], ids[i + 2])]++;
        }
        if (++lines % 10000000 == 0)
            std::fprintf(stderr, "%zu lines, %zu tokens, %zu trigrams\n", lines, tokens, tri.size());
    }

    size_t n1 = 0, n2 = 0;
    std::vector<std::pair<std::string, uint32_t>> kept;
    std::unordered_map<uint64_t, uint32_t> kept_ctx;
    const uint64_t m = (1u << 21) - 1;
    for (const auto& [k, c] : tri) {
        if (c == 1) n1++;
        if (c == 2) n2++;
        if (c < (uint64_t)min_count) continue;
        const uint64_t a = k >> 42, b = k >> 21 & m, d = k & m;
        kept.push_back({words[a] + " " + words[b] + " " + words[d], c});
        kept_ctx[key2(a, b)] = ctx[key2(a, b)];
    }
    std::sort(kept.begin(), kept.end());
    const double D = n1 + 2.0 * n2 > 0 ? n1 / (n1 + 2.0 * n2) : 0.5;

    FILE* f = std::fopen(argv[3], "w");
    std::fprintf(f, "# discount %.4f\n", D);
    for (const auto& [s, c] : kept) std::fprintf(f, "%s %u\n", s.c_str(), c);
    std::fclose(f);
    std::vector<std::pair<std::string, uint32_t>> cs;
    for (const auto& [k, c] : kept_ctx) cs.push_back({words[k >> 21] + " " + words[k & m], c});
    std::sort(cs.begin(), cs.end());
    f = std::fopen(argv[4], "w");
    for (const auto& [s, c] : cs) std::fprintf(f, "%s %u\n", s.c_str(), c);
    std::fclose(f);
    std::fprintf(stderr, "%zu lines, %zu tokens, %zu distinct trigrams (n1 %zu, n2 %zu, D %.4f); "
                 "kept %zu trigrams (count >= %ld) in %zu contexts\n",
                 lines, tokens, tri.size(), n1, n2, D, kept.size(), min_count, cs.size());
    return 0;
}
