#pragma once
#include "tables.hpp"
#include <string>
#include <vector>

// ── Vigenère primitives ───────────────────────────────────────────────────────

inline char vig_enc(char p, char k) { return 'a' + (p - 'a' + k - 'a') % ALPHA; }
inline char vig_dec(char c, char k) { return 'a' + (c - k + ALPHA) % ALPHA; }

inline std::string vig_decrypt(const std::string& cipher, const std::string& key)
{
    std::string plain(cipher.size(), 'a');
    for (size_t i = 0; i < cipher.size(); i++) plain[i] = vig_dec(cipher[i], key[i % key.size()]);
    return plain;
}

// ── Scoring ───────────────────────────────────────────────────────────────────
//
// The most probable split of `text` into dictionary words under the unigram word model,
// by dynamic programming.  Every candidate from the search splits fully into words; a
// letter outside any word ("stray") costs STRAY_LOGP, below any word, so it is only used
// when the most probable split prefers it.  Returns the log-probability and, if `fmt` is
// given, the split as "[the:1][cat:2]...".

static constexpr double STRAY_LOGP = -15.0;

inline double segment(const Dictionary& d, const std::string& text, std::string* fmt = nullptr)
{
    const int n = (int)text.size();
    std::vector<double> dp(n + 1, -1e300);
    std::vector<int>    back(n + 1, 0);    // length of the word ending here, 0 = stray
    dp[0] = 0;
    for (int i = 0; i < n; i++) {
        if (dp[i] + STRAY_LOGP > dp[i + 1]) { dp[i + 1] = dp[i] + STRAY_LOGP; back[i + 1] = 0; }
        int u = 0;
        for (int e = i; e < n; e++) {
            if ((u = d.trie.ch[u][text[e] - 'a']) < 0) break;
            if (d.trie.mark[u] && dp[i] + d.node_logp[u] > dp[e + 1]) {
                dp[e + 1] = dp[i] + d.node_logp[u];
                back[e + 1] = e + 1 - i;
            }
        }
    }
    if (fmt) {
        fmt->clear();
        for (int i = n; i > 0; ) {
            int l = back[i];
            if (l == 0) { *fmt = text[i - 1] + *fmt; i--; continue; }
            std::string w = text.substr(i - l, l);
            *fmt = "[" + w + ":" + std::to_string(d.rank.at(w)) + "]" + *fmt;
            i -= l;
        }
    }
    return dp[n];
}

// Ranking score of a (key, plaintext) candidate: log P(plaintext) + log P(key).  A key is a
// priori one of 26^kl strings, so a fluent plaintext from a short key is stronger evidence
// than the same from a long one.
inline double candidate_score(const Dictionary& d, const std::string& plain, int key_len)
{
    return segment(d, plain) - key_len * std::log((double)ALPHA);
}
