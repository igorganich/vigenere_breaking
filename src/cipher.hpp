#pragma once
#include "tables.hpp"
#include <string>
#include <vector>

// ── Vigenere primitives ───────────────────────────────────────────────────────

inline char vig_enc(char p, char k) { return 'a' + (p - 'a' + k - 'a') % 26; }
inline char vig_dec(char c, char k) { return 'a' + (c - k + 26)         % 26; }

inline std::string vig_decrypt(const std::string& cipher, const std::string& key)
{
    std::string plain(cipher.size(), '\0');
    int klen = (int)key.size();
    for (int i = 0; i < (int)cipher.size(); i++)
        plain[i] = vig_dec(cipher[i], key[i % klen]);
    return plain;
}

// ── Tetragram fitness (no heap allocation) ────────────────────────────────────
//
// Computes average log-probability of 4-grams in the candidate decryption.
// Works directly on raw cipher/key bytes so the hot loop stays allocation-free.

inline double fitness(const char* cipher, int clen,
                      const char* key,    int klen,
                      const std::array<double, QUAD_LEN>& quad)
{
    int n = clen - 3;
    if (n <= 0) return NO_LOG;

    // Precompute key[i % klen]-'a' for all positions to avoid repeated modulo
    // in the inner loop (klen ≤ 12, clen ≤ ~1000, so stack is fine).
    int kv[1024];
    for (int i = 0; i < clen; i++) kv[i] = key[i % klen] - 'a';

    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        int a = cipher[i  ] - 'a' - kv[i  ] + 26; if (a >= 26) a -= 26;
        int b = cipher[i+1] - 'a' - kv[i+1] + 26; if (b >= 26) b -= 26;
        int c = cipher[i+2] - 'a' - kv[i+2] + 26; if (c >= 26) c -= 26;
        int d = cipher[i+3] - 'a' - kv[i+3] + 26; if (d >= 26) d -= 26;
        sum += quad[a*17576 + b*676 + c*26 + d];
    }
    return sum / (double)n;
}

// ── Non-overlapping word segmentation ────────────────────────────────────────
//
// Finds the partition of 'text' into dictionary words that maximises the total
// number of characters covered, using dynamic programming.  Characters not
// covered by any word in the optimal partition are "stray" and shown as-is in
// the formatted output.
//
// Example: "algorithmdesign" → "[algorithm][design]", coverage = 1.0
//          "bhestriceofonif" → "b[he]str[ice][of][on][if]", coverage ≈ 0.80

struct WordSegment {
    double      coverage;    // fraction of chars belonging to non-overlapping words
    std::string formatted;   // e.g. "[the:1][man:42]x[ent:8103]"
    double      word_score;  // average rank of found words (lower = more common)
    int         word_count = 0; // number of non-overlapping words found
};

inline WordSegment word_segment(const std::string& text, const Tables& t)
{
    int n = (int)text.size();
    // dp[i] = max chars covered in text[0..i-1] by non-overlapping words
    std::vector<int> dp(n+1, 0);
    // back[i] = {prev_end, word_len}: word of word_len ending at i, or {i-1,0} for stray
    struct Back { int prev, wlen; };
    std::vector<Back> back(n+1, {0,0});
    for (int i = 0; i <= n; i++) back[i] = {i-1, 0};

    int cap = std::min(t.max_word_len, n);
    for (int i = 1; i <= n; i++) {
        // stray char: inherit from previous
        dp[i]   = dp[i-1];
        back[i] = {i-1, 0};

        for (int l = 1; l <= std::min(i, cap); l++) {
            if (t.dict_set.count(text.substr(i-l, l))) {
                int val = dp[i-l] + l;
                if (val >= dp[i]) { dp[i] = val; back[i] = {i-l, l}; }
            }
        }
    }

    // Backtrack
    std::string fmt;
    double rank_sum = 0.0; int word_count = 0;
    int i = n;
    // Build segments in reverse, then reverse the whole string
    while (i > 0) {
        auto [prev, wlen] = back[i];
        if (wlen > 0) {
            std::string word = text.substr(prev, wlen);
            auto it = t.word_rank.find(word);
            int rank = (it != t.word_rank.end()) ? it->second : (int)t.word_rank.size() + 1;
            fmt = "[" + word + ":" + std::to_string(rank) + "]" + fmt;
            rank_sum += rank; word_count++;
            i = prev;
        } else {
            fmt = text[i-1] + fmt;
            i--;
        }
    }

    double ws = word_count > 0 ? rank_sum / word_count : 0.0;
    return {(double)dp[n] / n, fmt, ws, word_count};
}
