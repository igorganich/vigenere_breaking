#include "crack.hpp"
#include "cipher.hpp"
#include <random>
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <omp.h>

// ── Tuning constants ──────────────────────────────────────────────────────────

static constexpr int    MAX_KEY_LEN             = 12;
static constexpr double IOC_THRESHOLD           = 1.6;
static constexpr int    DICT_OUTER_ITERS        = 700;
static constexpr int    DICT_POOL_SIZE          = 40;
static constexpr int    DICT_RESTARTS           = 4;
static constexpr int    LONG_KEY_SPLIT          = 7;
static constexpr int    OMP_THREADS             = 4;
static constexpr int    VAR_ITERS_PER_POS       = 1000;
static constexpr int    VAR_RESTARTS            = 3;
static constexpr int    LONG_TEXT_MIN           = 30;
// Pre-filter gate for word-hypothesis validation: skip check_if_words when
// the derived key produces fitness below this threshold (pure garbage).
static constexpr double WORD_SEARCH_FITNESS_GATE     = -18.0;
// Stricter gate for two_word_beam Phase 2: a combined (2-word) hypothesis that gives
// fitness below this is almost certainly garbage, even if the beam entry was decent.
// "thequickbrownfox" scores ~-12; common English is -9 to -13.

// ── Internal raw candidate (plain + key, before segmentation) ─────────────────

using Raw = std::pair<std::string, std::string>; // (plain, key)
using Raws = std::vector<Raw>;

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline int qidx(char a, char b, char c, char d) {
    return (a-'a')*17576 + (b-'a')*676 + (c-'a')*26 + (d-'a');
}

static double ioc(const std::string& text)
{
    int counts[ALPHA] = {};
    for (char c : text) counts[c-'a']++;
    long numer = 0, n = (long)text.size();
    for (int c : counts) numer += (long)c*(c-1);
    return n <= 1 ? 0.0 : (double)(ALPHA*numer)/(double)(n*(n-1));
}

static int guess_key_len(const std::string& cipher)
{
    for (int p = 1; p <= MAX_KEY_LEN; p++) {
        double sum = 0.0;
        for (int s = 0; s < p; s++) {
            std::string sl;
            for (int i = s; i < (int)cipher.size(); i += p) sl += cipher[i];
            sum += ioc(sl);
        }
        if (sum/p > IOC_THRESHOLD) return p;
    }
    return 0;
}

// ── Candidate pool ────────────────────────────────────────────────────────────

struct Pool {
    struct Entry { double f; std::string key; };
    std::vector<Entry> entries;
    int cap;

    Pool(int capacity, int keylen) : cap(capacity) {
        entries.reserve(capacity+1);
        entries.push_back({std::numeric_limits<double>::lowest(), std::string(keylen,'a')});
    }

    double worst_f() const { return entries.back().f; }

    void try_insert(double f, std::string key) {
        if (f <= entries.back().f) return;
        auto it = entries.begin();
        while (it != entries.end() && it->f > f) ++it;
        entries.insert(it, {f, std::move(key)});
        if ((int)entries.size() > cap) entries.pop_back();
    }

    void merge(Pool& other) {
        for (auto& e : other.entries) try_insert(e.f, e.key);
    }

    int size() const { return (int)entries.size(); }

    // Return all pool keys whose decryption passes the word-coverage filter.
    Raws validate(const std::string& cipher, const Tables& t) const {
        Raws out;
        for (const auto& e : entries) {
            auto plain = vig_decrypt(cipher, e.key);
            if (check_if_words(plain, t)) out.push_back({plain, e.key});
        }
        return out;
    }
};

// ── Variational hill-climb with incremental fitness ───────────────────────────

struct VarState {
    std::vector<char> plain;
    double            total;
    int               n;

    void init(const char* cp, int clen, const std::string& key,
              const std::array<double,QUAD_LEN>& quad) {
        n = clen-3;
        int klen = (int)key.size();
        plain.resize(clen);
        for (int i = 0; i < clen; i++) plain[i] = vig_dec(cp[i], key[i%klen]);
        total = 0;
        for (int i = 0; i < n; i++)
            total += quad[qidx(plain[i],plain[i+1],plain[i+2],plain[i+3])];
    }
};

static void try_all_at_pos(const char* cp, int clen, std::string& key,
                            VarState& vs, int pos,
                            const std::array<double,QUAD_LEN>& quad)
{
    int klen = (int)key.size();
    if (klen >= 4) {
        struct ATgm { int j, which, ci; };
        ATgm buf[512]; int nb = 0;
        double old_sum = 0.0;
        for (int i = pos; i < clen; i += klen)
            for (int j = std::max(0,i-3); j <= std::min(vs.n-1,i); j++) {
                old_sum += quad[qidx(vs.plain[j],vs.plain[j+1],vs.plain[j+2],vs.plain[j+3])];
                buf[nb++] = {j, i-j, i};
            }
        if (nb == 0) return;

        double best_delta = 0.0; char best_c = key[pos];
        for (int ci = 0; ci < ALPHA; ci++) {
            char c = 'a'+ci; double ns = 0.0;
            for (int k = 0; k < nb; k++) {
                auto [j, which, idx] = buf[k];
                char q0=vs.plain[j],q1=vs.plain[j+1],q2=vs.plain[j+2],q3=vs.plain[j+3];
                char np = vig_dec(cp[idx], c);
                if      (which==0) q0=np;
                else if (which==1) q1=np;
                else if (which==2) q2=np;
                else               q3=np;
                ns += quad[qidx(q0,q1,q2,q3)];
            }
            double d = ns - old_sum;
            if (d > best_delta) { best_delta = d; best_c = c; }
        }
        if (best_c != key[pos]) {
            for (int i = pos; i < clen; i += klen) vs.plain[i] = vig_dec(cp[i], best_c);
            vs.total += best_delta; key[pos] = best_c;
        }
    } else {
        char saved = key[pos]; double best_f = vs.total; char best_c = saved;
        for (char c = 'a'; c <= 'z'; c++) {
            key[pos] = c;
            double f = fitness(cp, clen, key.data(), klen, quad) * vs.n;
            if (f > best_f) { best_f = f; best_c = c; }
        }
        key[pos] = best_c;
        if (best_c != saved) {
            for (int i = pos; i < clen; i += klen) vs.plain[i] = vig_dec(cp[i], best_c);
            vs.total = best_f;
        }
    }
}

static Raws crack_variational(
    const std::string& cipher, const Tables& t, int keylen, uint32_t seed)
{
    if (keylen <= 0) return {};
    const int clen = (int)cipher.size(); const char* cp = cipher.data();
    auto rng = std::mt19937{seed};
    std::uniform_int_distribution<int> pos_dist(0, keylen-1);

    std::string key(keylen,'a');
    VarState vs; vs.init(cp, clen, key, t.quad);

    for (int iter = 0; iter < VAR_ITERS_PER_POS*keylen; iter++)
        try_all_at_pos(cp, clen, key, vs, pos_dist(rng), t.quad);

    auto plain = vig_decrypt(cipher, key);
    if (check_if_words(plain, t)) return {{plain, key}};
    return {};
}

// ── Short-key: stochastic word-hypothesis hill-climb ──────────────────────────

static Raws stochastic_search(
    const std::string& cipher, const Tables& t, int keylen, uint32_t seed)
{
    const int clen = (int)cipher.size(); const char* cp = cipher.data();
    auto rng = std::mt19937{seed};
    std::uniform_int_distribution<int> pos_dist(0, clen-1);
    Pool pool(DICT_POOL_SIZE, keylen);

    for (int iter = 0; iter < DICT_OUTER_ITERS; iter++) {
        int x = pos_dist(rng);
        int pool_sz = pool.size();
        for (int pi = 0; pi < pool_sz; pi++) {
            const std::string base_key = pool.entries[pi].key;
            for (const auto& word : t.dict) {
                if (x+(int)word.size() > clen) continue;
                std::string k = base_key;
                for (int j = 0; j < (int)word.size(); j++)
                    k[(x+j)%keylen] = 'a'+(cp[x+j]-word[j]+26)%26;
                double f = fitness(cp, clen, k.data(), keylen, t.quad);
                pool.try_insert(f, std::move(k));
            }
        }
    }
    return pool.validate(cipher, t);
}

// Non-exhaustive: stops at the first kl that produces any hit (fast path).
// Exhaustive: tries all kl values and accumulates every candidate found.
static Raws crack_short_key(const std::string& cipher, const Tables& t, bool exhaustive)
{
    const int clen = (int)cipher.size();
    std::random_device rd;
    Raws all;
    for (int kl = 1; kl < LONG_KEY_SPLIT && kl < clen; kl++) {
        Raws kl_raws;
        #pragma omp parallel for schedule(dynamic) num_threads(DICT_RESTARTS)
        for (int r = 0; r < DICT_RESTARTS; r++) {
            uint32_t seed = rd() ^ ((uint32_t)kl*7919u + (uint32_t)r*31337u);
            auto part = stochastic_search(cipher, t, kl, seed);
            #pragma omp critical
            kl_raws.insert(kl_raws.end(), part.begin(), part.end());
        }
        all.insert(all.end(), kl_raws.begin(), kl_raws.end());
        if (!exhaustive && !kl_raws.empty()) break;
    }
    return all;
}

// ── Long-key: long-word exhaustive + 2-word beam ──────────────────────────────

// Long-word exhaustive: for each dictionary word of length >= keylen, derive the
// full key in one step via the consistency check.  Validate immediately — the
// pool approach is avoided because fitness ranking can evict the correct key
// when false-positive short-key hypotheses happen to score higher on quadgrams.
static Raws long_word_search(const std::string& cipher, const Tables& t, int keylen)
{
    const int clen = (int)cipher.size(); const char* cp = cipher.data();
    Raws results;

    #pragma omp parallel for num_threads(OMP_THREADS) schedule(static)
    for (int wi = 0; wi < (int)t.dict.size(); wi++) {
        const std::string& word = t.dict[wi];
        int wlen = (int)word.size();
        if (wlen < keylen) continue;
        for (int x = 0; x+wlen <= clen; x++) {
            char kbuf[MAX_KEY_LEN] = {}; bool set[MAX_KEY_LEN] = {}; bool ok = true;
            for (int j = 0; j < wlen && ok; j++) {
                int  kpos = (x+j)%keylen;
                char kval = 'a'+(cp[x+j]-word[j]+26)%26;
                if (set[kpos] && kbuf[kpos] != kval) { ok = false; break; }
                kbuf[kpos] = kval; set[kpos] = true;
            }
            if (!ok) continue;
            double f = fitness(cp, clen, kbuf, keylen, t.quad);
            if (f < WORD_SEARCH_FITNESS_GATE) continue;
            std::string key(kbuf, keylen);
            auto plain = vig_decrypt(cipher, key);
            if (check_if_words(plain, t)) {
                #pragma omp critical
                results.push_back({plain, std::move(key)});
            }
        }
    }
    return results;
}

// Word-seeded hill-climbing: seed the key from each (word, position) hypothesis,
// then hill-climb the remaining uncovered key positions using try_all_at_pos.
// This avoids beam ranking entirely — starting from a word that appears anywhere
// in the actual plaintext will produce a strong signal that the hill-climb can
// follow to the correct key, even without knowing which word is "first".
static Raws word_seed_hillclimb(const std::string& cipher, const Tables& t, int keylen)
{
    const int clen = (int)cipher.size(); const char* cp = cipher.data();
    Raws results;

    #pragma omp parallel for num_threads(OMP_THREADS) schedule(static)
    for (int wi = 0; wi < (int)t.dict.size(); wi++) {
        const std::string& word = t.dict[wi];
        int wlen = (int)word.size();
        if (wlen >= keylen) continue; // long words are handled by long_word_search

        for (int x = 0; x + wlen <= clen; x++) {
            std::string key(keylen, 'a');
            bool covered[MAX_KEY_LEN] = {};
            for (int j = 0; j < wlen; j++) {
                int kpos = (x+j)%keylen;
                key[kpos] = 'a' + (cp[x+j] - word[j] + 26) % 26;
                covered[kpos] = true;
            }

            VarState vs; vs.init(cp, clen, key, t.quad);

            // Two passes: first pass fills uncovered positions, second pass lets each
            // position reconsider given the others are now set.  For two uncovered
            // positions this reliably escapes the local optima that a single greedy
            // pass produces (e.g. first pass picks wrong key[1] because key[2]='a';
            // second pass corrects it once key[2] is known).
            for (int pass = 0; pass < 2; pass++)
                for (int kpos = 0; kpos < keylen; kpos++)
                    if (!covered[kpos]) try_all_at_pos(cp, clen, key, vs, kpos, t.quad);

            if (vs.total / (double)(clen-3) < WORD_SEARCH_FITNESS_GATE) continue;
            auto plain = vig_decrypt(cipher, key);
            if (check_if_words(plain, t)) {
                #pragma omp critical
                results.push_back({plain, std::move(key)});
            }
        }
    }
    return results;
}

// ── Word cover search: recursive CSP word placement ──────────────────────────
// Recursively places dictionary words starting at the leftmost uncovered
// cipher position, propagating key constraints at each step.  Stronger than
// long_word_search (which needs one word ≥ kl) because multiple shorter words
// collectively cover the full key.  Only viable for short ciphertexts where
// the branching factor stays manageable.

// full=false: bounded search — min_L heuristic at root + result/node caps.
// full=true:  exhaustive — every valid word covering is found; may be slow.
static void wcs_recurse(
    const std::string& cipher, int kl,
    std::vector<int>&  key_buf,
    std::vector<bool>& covered,
    const std::vector<std::vector<std::string>>& wbl,
    Raws& results,
    int& result_limit,   // decremented per result;  -1 = unlimited
    int& node_budget,    // decremented per call;     -1 = unlimited
    int  min_L)          // minimum word length at this level
{
    if (result_limit == 0 || node_budget == 0) return;
    if (node_budget > 0) --node_budget;

    const int n = (int)cipher.size();
    int p = -1;
    for (int i = 0; i < n; i++) { if (!covered[i]) { p = i; break; } }

    if (p == -1) {
        std::string key(kl, 'a');
        for (int i = 0; i < kl; i++) key[i] = 'a' + key_buf[i];
        std::string plain(n, '?');
        for (int i = 0; i < n; i++)
            plain[i] = 'a' + (cipher[i] - 'a' - key_buf[i % kl] + 26) % 26;
        results.push_back({plain, key});
        if (result_limit > 0) --result_limit;
        return;
    }

    const int max_L = std::min((int)wbl.size() - 1, n - p);
    for (int L = min_L; L <= max_L; L++) {
        for (const auto& word : wbl[L]) {
            if (result_limit == 0 || node_budget == 0) return;

            bool ok = true;
            std::vector<int> new_kpos;
            new_kpos.reserve(L);

            for (int j = 0; j < L && ok; j++) {
                int kpos = (p + j) % kl;
                int kval = (cipher[p + j] - 'a' - (word[j] - 'a') + 26) % 26;
                if (key_buf[kpos] == -1) {
                    key_buf[kpos] = kval;
                    new_kpos.push_back(kpos);
                } else if (key_buf[kpos] != kval) {
                    ok = false;
                }
            }

            if (!ok) {
                for (int kp : new_kpos) key_buf[kp] = -1;
                continue;
            }

            for (int j = 0; j < L; j++) covered[p + j] = true;
            wcs_recurse(cipher, kl, key_buf, covered, wbl, results, result_limit, node_budget, 2);
            for (int j = 0; j < L; j++) covered[p + j] = false;
            for (int kp : new_kpos) key_buf[kp] = -1;
        }
    }
}

static Raws word_cover_search(
    const std::string& cipher, int kl,
    const std::vector<std::vector<std::string>>& wbl,
    const WcsParams& p)
{
    const int n = (int)cipher.size();
    std::vector<int>  key_buf(kl, -1);
    std::vector<bool> covered(n, false);
    Raws results;

    // 0 → -1 (unlimited) for the internal sentinel
    int result_limit = (p.results == 0) ? -1 : p.results;
    int node_budget  = (p.budget  == 0) ? -1 : p.budget;
    int min_L_root   = (p.min_word == -1) ? std::max(2, kl - 1) : std::max(2, p.min_word);

    wcs_recurse(cipher, kl, key_buf, covered, wbl, results, result_limit, node_budget, min_L_root);
    return results;
}

static bool any_high_coverage(const Raws& raws, const Tables& t, double thr = 0.90) {
    for (auto& [plain, key] : raws)
        if (word_segment(plain, t).coverage >= thr) return true;
    return false;
}

static constexpr int WCS_MAX_CLEN = 25;

// Non-exhaustive: tries kl=LONG_KEY_SPLIT..MAX_KEY_LEN, stops on first high-coverage hit.
// Exhaustive: starts from kl=1 so the word-based search covers all key lengths,
//             no early exits, collects everything.
static Raws crack_long_key(const std::string& cipher, const Tables& t, bool exhaustive,
                           const WcsParams& wcs)
{
    const int clen = (int)cipher.size();
    const int start_kl = exhaustive ? 1 : LONG_KEY_SPLIT;

    // Precompute words-by-length once for word_cover_search (short ciphertexts only)
    std::vector<std::vector<std::string>> wbl;
    if (clen <= WCS_MAX_CLEN) {
        wbl.resize(t.max_word_len + 1);
        for (const auto& w : t.dict_full) {
            int L = (int)w.size();
            if (L >= 2 && L <= t.max_word_len) wbl[L].push_back(w);
        }
    }

    Raws all;
    for (int kl = start_kl; kl <= std::min(MAX_KEY_LEN, clen - 1); kl++) {
        Raws kl_raws;
        auto lws = long_word_search(cipher, t, kl);
        kl_raws.insert(kl_raws.end(), lws.begin(), lws.end());

        auto wsh = word_seed_hillclimb(cipher, t, kl);
        kl_raws.insert(kl_raws.end(), wsh.begin(), wsh.end());

        if (exhaustive && !wbl.empty()) {
            auto r = word_cover_search(cipher, kl, wbl, wcs);
            kl_raws.insert(kl_raws.end(), r.begin(), r.end());
        }

        all.insert(all.end(), kl_raws.begin(), kl_raws.end());
        if (!exhaustive && any_high_coverage(kl_raws, t)) break;
    }
    return all;
}

// ── Top-level: aggregate all methods, deduplicate, segment ───────────────────

std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& t,
                                  KeyPriority priority, WcsParams wcs)
{
    const bool exhaustive = (priority == KeyPriority::EXHAUSTIVE);
    Raws raw;

    // Variational for longer ciphertexts (key-length agnostic, always useful)
    if ((int)cipher.size() >= LONG_TEXT_MIN) {
        int kl = guess_key_len(cipher);
        std::random_device rd;
        for (int r = 0; r < VAR_RESTARTS; r++) {
            auto part = crack_variational(cipher, t, kl, rd());
            raw.insert(raw.end(), part.begin(), part.end());
        }
    }

    if (exhaustive) {
        auto lk = crack_long_key(cipher, t, true, wcs);
        raw.insert(raw.end(), lk.begin(), lk.end());
    } else if (priority != KeyPriority::SHORT_ONLY) {
        auto lk = crack_long_key(cipher, t, false, wcs);
        raw.insert(raw.end(), lk.begin(), lk.end());

        bool skip_short = priority == KeyPriority::LONG_FIRST && any_high_coverage(lk, t);
        if (!skip_short) {
            auto sk = crack_short_key(cipher, t, false);
            raw.insert(raw.end(), sk.begin(), sk.end());
        }
    } else {
        auto sk = crack_short_key(cipher, t, false);
        raw.insert(raw.end(), sk.begin(), sk.end());
    }

    // Deduplicate by key, compute fitness + segmentation for each unique candidate.
    static constexpr char NOOP_KEY[] = "a";
    std::unordered_set<std::string> seen;
    std::vector<CrackCandidate> results;
    for (auto& [plain, key] : raw) {
        if (seen.insert(key).second) {
            double fit = fitness(plain.data(), (int)plain.size(), NOOP_KEY, 1, t.quad);
            results.push_back({plain, key, fit, word_segment(plain, t)});
        }
    }

    if (exhaustive) {
        // Exhaustive mode: coverage desc → key length asc → word count asc → word_rank asc.
        std::sort(results.begin(), results.end(),
                  [](const CrackCandidate& a, const CrackCandidate& b){
                      if (a.segment.coverage != b.segment.coverage)
                          return a.segment.coverage > b.segment.coverage;
                      if (a.key.size() != b.key.size())
                          return a.key.size() < b.key.size();
                      if (a.segment.word_count != b.segment.word_count)
                          return a.segment.word_count < b.segment.word_count;
                      return a.segment.word_score < b.segment.word_score;
                  });
        results.erase(
            std::remove_if(results.begin(), results.end(),
                           [](const CrackCandidate& c){ return c.segment.coverage < 0.90; }),
            results.end());
    } else {
        // Default: coverage descending, fitness descending as tiebreak.
        std::sort(results.begin(), results.end(),
                  [](const CrackCandidate& a, const CrackCandidate& b){
                      if (a.segment.coverage != b.segment.coverage)
                          return a.segment.coverage > b.segment.coverage;
                      return a.fitness > b.fitness;
                  });
    }

    return results;
}
