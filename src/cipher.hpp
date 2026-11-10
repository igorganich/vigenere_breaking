#pragma once
#include "model.hpp"
#include "tables.hpp"
#include <string>
#include <vector>

// ── Vigenère primitives ───────────────────────────────────────────────────────

// Letters in internal form (tables.hpp); `n` is the alphabet size (26, or 33 for Ukrainian).
inline char vig_enc(char p, char k, int n) { return letter_char((letter_index(p) + letter_index(k)) % n); }
inline char vig_dec(char c, char k, int n) { return letter_char((letter_index(c) - letter_index(k) + n) % n); }

inline std::string vig_decrypt(const std::string& cipher, const std::string& key, int n)
{
    std::string plain(cipher.size(), 'a');
    for (size_t i = 0; i < cipher.size(); i++) plain[i] = vig_dec(cipher[i], key[i % key.size()], n);
    return plain;
}

// ── Scoring ───────────────────────────────────────────────────────────────────
//
// The most probable split of `text` into dictionary words under the language model, by
// dynamic programming.  Every candidate from the search splits fully into words; a letter
// outside any word ("stray") costs m.stray_logp, below any word, so it is only used when
// the most probable split prefers it.  Returns the log-probability and, if `fmt` is given,
// the split as "[the:1][cat:2]..." (word:frequency rank).

namespace scoring_detail {

inline void word_fmt(const Dictionary& d, const std::string& w, std::string* out)
{
    *out = "[" + w + ":" + std::to_string(d.rank.at(w)) + "]" + *out;
}

// Unigram model: the state is the position.
inline double segment_unigram(const Dictionary& d, const LanguageModel& m, const std::string& text,
                              std::string* fmt)
{
    const int n = (int)text.size();
    std::vector<double> dp(n + 1, -1e300);
    std::vector<int>    back(n + 1, 0);    // length of the word ending here, 0 = stray
    dp[0] = 0;
    for (int i = 0; i < n; i++) {
        if (dp[i] + m.stray_logp > dp[i + 1]) { dp[i + 1] = dp[i] + m.stray_logp; back[i + 1] = 0; }
        int u = 0;
        for (int e = i; e < n; e++) {
            if ((u = d.trie.ch[u][letter_index(text[e])]) < 0) break;
            if (d.trie.mark[u] && dp[i] + m.uni[u] > dp[e + 1]) {
                dp[e + 1] = dp[i] + m.uni[u];
                back[e + 1] = e + 1 - i;
            }
        }
    }
    if (fmt) {
        fmt->clear();
        for (int i = n; i > 0; ) {
            int l = back[i];
            if (l == 0) { *fmt = text[i - 1] + *fmt; i--; continue; }
            word_fmt(d, text.substr(i - l, l), fmt);
            i -= l;
        }
    }
    return dp[n];
}

// Bigram model: the state is the last word, i.e. a span [start, end) of the text, or "no
// word" at a position (the start of the text, or right after a stray letter).
inline double segment_bigram(const Dictionary& d, const LanguageModel& m, const std::string& text,
                             std::string* fmt)
{
    struct Span { int start, end, node; };
    thread_local std::vector<Span>   spans;
    thread_local std::vector<int>    first, ends, end_first, fill;
    thread_local std::vector<double> V, C;
    thread_local std::vector<int>    backV, backC;
    const int n = (int)text.size();

    // Every dictionary word in the text, grouped by start; then the same grouped by end.
    spans.clear();
    first.assign(n + 2, 0);
    for (int i = 0; i < n; i++) {
        first[i] = (int)spans.size();
        int u = 0;
        for (int e = i; e < n; e++) {
            if ((u = d.trie.ch[u][letter_index(text[e])]) < 0) break;
            if (d.trie.mark[u]) spans.push_back({i, e + 1, u});
        }
    }
    first[n] = first[n + 1] = (int)spans.size();
    end_first.assign(n + 2, 0);
    for (const auto& s : spans) end_first[s.end + 1]++;
    for (int e = 0; e <= n; e++) end_first[e + 1] += end_first[e];
    ends.resize(spans.size());
    fill.assign(end_first.begin(), end_first.end());
    for (int s = 0; s < (int)spans.size(); s++) ends[fill[spans[s].end]++] = s;

    V.assign(spans.size(), -1e300);  backV.assign(spans.size(), -1);   // -1: after "no word"
    C.assign(n + 1, -1e300);         backC.assign(n + 1, -1);          // -1: stray after C[e-1]
    C[0] = 0;
    for (int e = 0; e < n; e++) {
        // A stray letter at e, after the best state ending at e.
        double best = C[e];
        int    arg  = -1;
        for (int k = end_first[e]; k < end_first[e + 1]; k++)
            if (V[ends[k]] > best) { best = V[ends[k]]; arg = ends[k]; }
        if (best + m.stray_logp > C[e + 1]) { C[e + 1] = best + m.stray_logp; backC[e + 1] = arg; }
        // A word starting at e, after "no word" or after each word ending at e.
        for (int t = first[e]; t < first[e + 1]; t++) {
            const int v = spans[t].node;
            double val = C[e] + m.uni[v];
            int    bp  = -1;
            for (int k = end_first[e]; k < end_first[e + 1]; k++) {
                const int s = ends[k], u = spans[s].node;
                const double  lp = m.bi(u, v);
                if (V[s] + lp > val) { val = V[s] + lp; bp = s; }
            }
            V[t] = val; backV[t] = bp;
        }
    }
    double best = C[n];
    int    arg  = -1;
    for (int k = end_first[n]; k < end_first[n + 1]; k++)
        if (V[ends[k]] > best) { best = V[ends[k]]; arg = ends[k]; }

    if (fmt) {
        fmt->clear();
        int pos = n, s = arg;      // state: span s, or "no word" at pos when s < 0
        while (pos > 0) {
            if (s >= 0) {
                word_fmt(d, text.substr(spans[s].start, spans[s].end - spans[s].start), fmt);
                pos = spans[s].start;
                s   = backV[s];
            } else {
                *fmt = text[pos - 1] + *fmt;
                s = backC[pos];
                pos--;
            }
        }
    }
    return best;
}

// Trigram model: the state is the last two words, i.e. a span t and the span before it (or
// "no word" when t follows the start of the text or a stray letter).
inline double segment_trigram(const Dictionary& d, const LanguageModel& m, const std::string& text,
                              std::string* fmt)
{
    struct Span { int start, end, node; };
    thread_local std::vector<Span>   spans;
    thread_local std::vector<int>    first, ends, end_first, fill, pos, off, st_span, back;
    thread_local std::vector<double> V, C;
    thread_local std::vector<int>    backC;
    const int n = (int)text.size();

    spans.clear();
    first.assign(n + 2, 0);
    for (int i = 0; i < n; i++) {
        first[i] = (int)spans.size();
        int u = 0;
        for (int e = i; e < n; e++) {
            if ((u = d.trie.ch[u][letter_index(text[e])]) < 0) break;
            if (d.trie.mark[u]) spans.push_back({i, e + 1, u});
        }
    }
    first[n] = first[n + 1] = (int)spans.size();
    const int S = (int)spans.size();
    end_first.assign(n + 2, 0);
    for (const auto& s : spans) end_first[s.end + 1]++;
    for (int e = 0; e <= n; e++) end_first[e + 1] += end_first[e];
    ends.resize(S);
    pos.resize(S);                     // index of a span within the spans ending where it ends
    fill.assign(end_first.begin(), end_first.end());
    for (int s = 0; s < S; s++) {
        pos[s] = fill[spans[s].end] - end_first[spans[s].end];
        ends[fill[spans[s].end]++] = s;
    }
    // States of span t: off[t] = (t, no word), off[t] + 1 + pos[s] = (t, s) for s ending at
    // t's start.
    off.resize(S + 1);
    off[0] = 0;
    for (int t = 0; t < S; t++) {
        const int b = spans[t].start;
        off[t + 1] = off[t] + 1 + (end_first[b + 1] - end_first[b]);
    }
    const int N = off[S];
    st_span.resize(N);
    for (int t = 0; t < S; t++) for (int x = off[t]; x < off[t + 1]; x++) st_span[x] = t;
    V.assign(N, -1e300);  back.assign(N, -1);      // back: previous state, -1 = from C[start]
    C.assign(n + 1, -1e300);  backC.assign(n + 1, -1);  // backC: state before the stray, -1 = C[e-1]
    C[0] = 0;

    for (int e = 0; e < n; e++) {
        // A stray letter at e, after the best state ending at e.
        double best = C[e];
        int    arg  = -1;
        for (int k = end_first[e]; k < end_first[e + 1]; k++)
            for (int x = off[ends[k]]; x < off[ends[k] + 1]; x++)
                if (V[x] > best) { best = V[x]; arg = x; }
        if (best + m.stray_logp > C[e + 1]) { C[e + 1] = best + m.stray_logp; backC[e + 1] = arg; }
        // A word t starting at e.
        for (int t = first[e]; t < first[e + 1]; t++) {
            const int w = spans[t].node;
            V[off[t]] = C[e] + m.uni[w];
            for (int k = end_first[e]; k < end_first[e + 1]; k++) {
                const int s = ends[k], v = spans[s].node;
                const int to = off[t] + 1 + pos[s];
                double val = V[off[s]] + m.bi(v, w);            // s had no word before it
                int    bp  = off[s];
                const int* before = &ends[end_first[spans[s].start]];   // spans ending at s's start
                for (int x = off[s] + 1; x < off[s + 1]; x++) {
                    const double c = V[x] + m.tri(spans[before[x - off[s] - 1]].node, v, w);
                    if (c > val) { val = c; bp = x; }
                }
                V[to] = val; back[to] = bp;
            }
        }
    }
    double best = C[n];
    int    arg  = -1;
    for (int k = end_first[n]; k < end_first[n + 1]; k++)
        for (int x = off[ends[k]]; x < off[ends[k] + 1]; x++)
            if (V[x] > best) { best = V[x]; arg = x; }

    if (fmt) {
        fmt->clear();
        int p = n, x = arg;        // state x, or "no word" at p when x < 0
        while (p > 0) {
            if (x >= 0) {
                const auto& sp = spans[st_span[x]];
                word_fmt(d, text.substr(sp.start, sp.end - sp.start), fmt);
                p = sp.start;
                x = x == off[st_span[x]] ? -1 : back[x];
            } else {
                *fmt = text[p - 1] + *fmt;
                x = backC[p];
                p--;
            }
        }
    }
    return best;
}

} // namespace scoring_detail

inline double segment(const Dictionary& d, const LanguageModel& m, const std::string& text,
                      std::string* fmt = nullptr)
{
    switch (m.kind) {
    case ModelKind::Bigram:  return scoring_detail::segment_bigram(d, m, text, fmt);
    case ModelKind::Trigram: return scoring_detail::segment_trigram(d, m, text, fmt);
    default:                 return scoring_detail::segment_unigram(d, m, text, fmt);
    }
}

// Ranking score of a (key, plaintext) candidate: log P(plaintext) + log P(key).  A key is a
// priori one of A^kl strings (A letters), so a fluent plaintext from a short key is stronger evidence
// than the same from a long one.
inline double candidate_score(const Dictionary& d, const LanguageModel& m, const std::string& plain,
                              int key_len, std::string* fmt = nullptr)
{
    return segment(d, m, plain, fmt) - key_len * std::log((double)d.alphabet.size());
}
