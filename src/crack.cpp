#include "crack.hpp"
#include "cipher.hpp"
#include <algorithm>
#include <unordered_set>

// ── Tuning constants ──────────────────────────────────────────────────────────

static constexpr int    MAX_KEY_LEN  = 12;
static constexpr double MIN_COVERAGE = 0.90; // drop candidates below this word coverage

// ── Internal raw candidate (plain + key, before segmentation) ─────────────────

using Raw = std::pair<std::string, std::string>; // (plain, key)
using Raws = std::vector<Raw>;

// ── Word cover search: recursive CSP word placement ──────────────────────────
// Recursively places dictionary words starting at the leftmost uncovered
// cipher position, propagating key constraints at each step.  Multiple short
// words collectively pin down the full key, so no single word has to span it.
// Only viable for short ciphertexts where the branching factor stays manageable.
//
// The result cap applies separately to each first-word length.  With one shared
// cap, on very short texts the coverings starting with one word length fill it
// before the true first word (shorter or longer) is ever tried.

static void wcs_recurse(
    const std::string& cipher, int kl,
    std::vector<int>&  key_buf,
    std::vector<bool>& covered,
    const std::vector<std::vector<std::string>>& wbl,
    Raws& results,
    int& result_limit,   // decremented per result;  -1 = unlimited
    int& node_budget,    // decremented per call;     -1 = unlimited
    int  min_L,          // minimum word length at this level
    int  max_L_cap)      // maximum word length at this level (-1 = no limit)
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

    int max_L = std::min((int)wbl.size() - 1, n - p);
    if (max_L_cap >= 0) max_L = std::min(max_L, max_L_cap);
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
            wcs_recurse(cipher, kl, key_buf, covered, wbl, results, result_limit, node_budget, 1, -1);
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
    int node_budget  = (p.budget  == 0) ? -1 : p.budget;
    int min_L_root   = (p.min_word == -1) ? std::max(2, kl - 1) : std::max(2, p.min_word);
    int max_L_root   = std::min((int)wbl.size() - 1, n);

    for (int L = min_L_root; L <= max_L_root && node_budget != 0; L++) {
        int result_limit = (p.results == 0) ? -1 : p.results;
        wcs_recurse(cipher, kl, key_buf, covered, wbl, results, result_limit, node_budget, L, L);
    }
    return results;
}

// ── Top-level: search every key length, deduplicate, segment, rank ───────────

std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& t, WcsParams wcs)
{
    const int clen = (int)cipher.size();

    std::vector<std::vector<std::string>> wbl(t.max_word_len + 1);
    for (const auto& w : t.dict_full) {
        int L = (int)w.size();
        if (L <= t.max_word_len) wbl[L].push_back(w);
    }

    Raws raw;
    for (int kl = 1; kl <= std::min(MAX_KEY_LEN, clen - 1); kl++) {
        auto r = word_cover_search(cipher, kl, wbl, wcs);
        raw.insert(raw.end(), r.begin(), r.end());
    }

    // ── Deduplicate by key, compute fitness + segmentation ────────────────────
    static constexpr char NOOP_KEY[] = "a";
    std::unordered_set<std::string> seen;
    std::vector<CrackCandidate> results;
    for (auto& [plain, key] : raw) {
        if (seen.insert(key).second) {
            double fit = fitness(plain.data(), (int)plain.size(), NOOP_KEY, 1, t.quad);
            results.push_back({plain, key, fit, word_segment(plain, t)});
        }
    }

    // ── Sort and filter: coverage ↓ → key length ↑ → word count ↑ → word rank ↑
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
                       [](const CrackCandidate& c){ return c.segment.coverage < MIN_COVERAGE; }),
        results.end());

    return results;
}
