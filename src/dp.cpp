#include "dp.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

// ── Column DP over key letters ────────────────────────────────────────────────
//
// For key length kl the plaintext is cut into tracks of kl letters (t = 0..T-1).  Key
// letter j fixes the plaintext letter at offset j of every track, so the search goes column
// by column, j = 0..kl-1, choosing one key letter per column.  Each track runs the word
// recognizer on its own letters.  After column j the rest of the search depends only on
// where each track is inside its current word, not on the key letters that got it there,
// so the states of a column are deduplicated: countless key prefixes collapse into one
// state and the search never repeats work.
//
// Per-track state:
//   N mode, trie node u      inside a word that started in this track (u = 0: at a boundary)
//   S mode, suffix node v    reading the tail of a word that started in an earlier track
//   pending                  set when an S-mode tail was closed: the tail's suffix node,
//                            checked at the end against the head the previous track left
// Words spanning track boundaries are verified once all columns are done (accept()).
//
// Keys are then read off by walking the columns again, keeping only states that can still
// reach an accepted end and carrying, for each key prefix, the set of states it can be in;
// so every key comes out exactly once.

uint64_t pack_key(const std::string& key)
{
    uint64_t code = 0, p = 1;
    for (char c : key) { code += (uint64_t)(c - 'a') * p; p *= ALPHA; }
    return code;
}

std::string unpack_key(uint64_t code, int key_len)
{
    std::string k(key_len, 'a');
    for (int i = 0; i < key_len; i++) { k[i] = 'a' + code % ALPHA; code /= ALPHA; }
    return k;
}

void parallel_for(size_t n, int threads, const std::function<void(size_t)>& fn)
{
    if (threads <= 1 || n < 2) { for (size_t i = 0; i < n; i++) fn(i); return; }
    std::atomic<size_t> next{0};
    const size_t chunk = std::max<size_t>(1, n / ((size_t)threads * 64));
    std::vector<std::thread> pool;
    std::exception_ptr error;      // the first exception of a worker (out of memory, disk full)
    std::mutex error_mu;
    for (int t = 0; t < threads; t++) pool.emplace_back([&]{
        try {
            for (size_t lo; (lo = next.fetch_add(chunk)) < n; )
                for (size_t i = lo; i < std::min(n, lo + chunk); i++) fn(i);
        } catch (...) {
            std::lock_guard<std::mutex> lock(error_mu);
            if (!error) error = std::current_exception();
            next = n;              // the other workers stop at their next chunk
        }
    });
    for (auto& th : pool) th.join();
    if (error) std::rethrow_exception(error);    // in the caller's thread, where it can be caught
}

namespace {

// Packed track state: mode (1 bit) | node (22 bits) | pending + 1 (22 bits)
enum Mode : uint64_t { N = 0, S = 1 };
inline uint64_t pack(uint64_t mode, uint64_t node, int64_t pend) {
    return mode | (node << 1) | ((uint64_t)(pend + 1) << 23);
}
inline uint64_t mode_of(uint64_t s) { return s & 1; }
inline int      node_of(uint64_t s) { return (int)((s >> 1) & ((1u << 22) - 1)); }
inline int      pend_of(uint64_t s) { return (int)(s >> 23) - 1; }

struct Problem {
    const std::string& cipher;
    const Dictionary&  dict;
    int                n, kl, T;
    std::vector<int>   len;   // letters per track
};

// Successors of one track state after reading letter x (0..2 of them).
inline int advance(const Dictionary& d, uint64_t s, int x, uint64_t out[2])
{
    const int u = node_of(s), pend = pend_of(s);
    int k = 0;
    if (mode_of(s) == N) {
        int v = d.trie.ch[u][x];
        if (v < 0) return 0;
        out[k++] = pack(N, v, pend);
        if (d.trie.mark[v]) out[k++] = pack(N, 0, pend);       // end the word here
    } else {
        int v = d.suffixes.ch[u][x];
        if (v < 0) return 0;
        out[k++] = pack(S, v, -1);
        if (d.suffixes.mark[v]) out[k++] = pack(N, 0, v);      // close the tail here
    }
    return k;
}

template <int M> using State = std::array<uint64_t, M>;

// Words spanning track boundaries: verify the chain left to right.
template <int M>
bool accept(const Problem& P, const State<M>& st)
{
    std::string acc;   // open word carried across the boundary ("" = at a word boundary)
    for (int t = 0; t < P.T; t++) {
        const uint64_t s = st[t];
        const int  node = node_of(s), pend = pend_of(s);
        const bool started_tail = mode_of(s) == S || pend >= 0;
        if (t == 0 && started_tail) return false;
        if (!started_tail) {
            if (!acc.empty()) return false;              // previous word left unfinished
            acc = node == 0 ? "" : P.dict.trie.str(node);
            continue;
        }
        if (acc.empty()) return false;                   // tail with no head
        if (mode_of(s) == S) {                           // whole track is inside one word
            acc += P.dict.suffixes.str(node);
            continue;
        }
        if (!P.dict.words.count(acc + P.dict.suffixes.str(pend))) return false;
        acc = node == 0 ? "" : P.dict.trie.str(node);
    }
    return acc.empty();
}

template <int M, class F>
void successors(const Problem& P, const State<M>& s, int j, int key, F&& emit)
{
    uint64_t opt[M][2];
    int      cnt[M];
    for (int t = 0; t < P.T; t++) {
        if (j >= P.len[t]) { opt[t][0] = s[t]; cnt[t] = 1; continue; }   // track finished
        int x = (P.cipher[t * P.kl + j] - 'a' - key + ALPHA) % ALPHA;
        if ((cnt[t] = advance(P.dict, s[t], x, opt[t])) == 0) return;
    }
    const int last = P.T - 1;
    State<M> out{};
    std::function<void(int)> rec = [&](int t) {
        if (t == P.T) {
            // the last track ends at this column: it must stop at a word boundary
            if (j == P.len[last] - 1 && (mode_of(out[last]) != N || node_of(out[last]) != 0)) return;
            emit(out);
            return;
        }
        for (int i = 0; i < cnt[t]; i++) { out[t] = opt[t][i]; rec(t + 1); }
    };
    rec(0);
}

template <int M>
std::vector<State<M>> initial_states(const Problem& P)
{
    std::vector<State<M>> init;
    State<M> s{};
    s[0] = pack(N, 0, -1);
    std::function<void(int)> rec = [&](int t) {
        if (t == P.T) { init.push_back(s); return; }
        s[t] = pack(N, 0, -1); rec(t + 1);   // track starts at a word boundary
        s[t] = pack(S, 0, -1); rec(t + 1);   // track starts inside a word
    };
    rec(1);
    return init;
}

static constexpr size_t SINK_BATCH = 1 << 16;   // keys handed to the sink at a time

template <int M>
size_t column_dp(const Problem& P, int threads, size_t limit, const KeySink& sink, SearchStats* stats)
{
    // Forward: the distinct states after each column.  Successors are deduplicated in
    // batches so the raw list never grows far beyond the number of distinct states.
    std::vector<std::vector<State<M>>> layers{initial_states<M>(P)};
    for (int j = 0; j < P.kl; j++) {
        std::vector<State<M>> next;
        size_t clean = 0;   // next[0..clean) is sorted and unique
        auto compact = [&]{
            std::sort(next.begin() + clean, next.end());
            next.erase(std::unique(next.begin() + clean, next.end()), next.end());
            std::inplace_merge(next.begin(), next.begin() + clean, next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            clean = next.size();
        };
        for (const auto& s : layers.back())
            for (int k = 0; k < ALPHA; k++)
                successors<M>(P, s, j, k, [&](const State<M>& o){
                    next.push_back(o);
                    if (next.size() - clean > 8'000'000) compact();
                });
        compact();
        if (stats) stats->peak_states = std::max(stats->peak_states, next.size());
        if (next.empty()) return 0;
        layers.push_back(std::move(next));
    }

    // Backward: which states can still reach an accepted end.
    const int L = P.kl;
    auto index = [&](int j, const State<M>& s) {
        auto it = std::lower_bound(layers[j].begin(), layers[j].end(), s);
        return (it != layers[j].end() && *it == s) ? (long)(it - layers[j].begin()) : -1L;
    };
    std::vector<std::vector<char>> live(L + 1);
    live[L].resize(layers[L].size());
    parallel_for(layers[L].size(), threads, [&](size_t i){ live[L][i] = accept<M>(P, layers[L][i]); });
    for (int j = L - 1; j >= 0; j--) {
        live[j].assign(layers[j].size(), 0);
        parallel_for(layers[j].size(), threads, [&](size_t i){
            for (int k = 0; k < ALPHA && !live[j][i]; k++)
                successors<M>(P, layers[j][i], j, k, [&](const State<M>& o){
                    long x = index(j + 1, o);
                    if (x >= 0 && live[j + 1][x]) live[j][i] = 1;
                });
        });
    }

    // Keys: walk key prefixes carrying the set of live states each prefix can be in.
    // Split by the first key letter into 26 tasks; each hands its keys to the sink in batches.
    std::vector<long> start;
    for (size_t i = 0; i < layers[0].size(); i++) if (live[0][i]) start.push_back((long)i);
    if (start.empty()) return 0;
    uint64_t pow26[13] = {1};
    for (int i = 1; i <= 12; i++) pow26[i] = pow26[i - 1] * ALPHA;
    std::atomic<size_t> found{0};
    std::atomic<bool>   over{false};
    std::function<void(int, const std::vector<long>&, uint64_t, std::vector<uint64_t>&, int)> walk =
        [&](int j, const std::vector<long>& set_j, uint64_t code, std::vector<uint64_t>& out, int only) {
        if (j == L) {
            out.push_back(code);
            if (++found > limit) over = true;
            else if (out.size() == SINK_BATCH) { sink(out.data(), out.size()); out.clear(); }
            return;
        }
        for (int k = 0; k < ALPHA && !over; k++) {
            if (only >= 0 && k != only) continue;
            std::vector<long> nx;
            for (long i : set_j)
                successors<M>(P, layers[j][i], j, k, [&](const State<M>& o){
                    long x = index(j + 1, o);
                    if (x >= 0 && live[j + 1][x]) nx.push_back(x);
                });
            if (nx.empty()) continue;
            std::sort(nx.begin(), nx.end());
            nx.erase(std::unique(nx.begin(), nx.end()), nx.end());
            walk(j + 1, nx, code + (uint64_t)k * pow26[j], out, -1);
        }
    };
    parallel_for(ALPHA, threads, [&](size_t k){
        std::vector<uint64_t> out;
        out.reserve(SINK_BATCH);
        walk(0, start, 0, out, (int)k);
        if (!over && !out.empty()) sink(out.data(), out.size());
    });
    if (over) {
        if (stats) stats->over_limit = true;
        return 0;
    }
    return found;
}

// Short keys split the text into many tracks (2^(T-1) start states) but have a tiny key
// space, so a brute force over all keys is exact and fast.
size_t brute_force(const std::string& cipher, int kl, const Dictionary& d, int threads, size_t limit,
                   const KeySink& sink, SearchStats* stats)
{
    uint64_t total = 1;
    for (int i = 0; i < kl; i++) total *= ALPHA;
    const int n = (int)cipher.size();
    std::vector<char> ok(total, 0);
    parallel_for(total, threads, [&](size_t code){
        std::string key = unpack_key(code, kl), plain(n, 'a');
        for (int i = 0; i < n; i++) plain[i] = 'a' + (cipher[i] - key[i % kl] + ALPHA) % ALPHA;
        std::vector<char> reach(n + 1, 0);
        reach[0] = 1;
        for (int i = 0; i < n; i++) {
            if (!reach[i]) continue;
            int u = 0;
            for (int e = i; e < n; e++) {
                if ((u = d.trie.ch[u][plain[e] - 'a']) < 0) break;
                if (d.trie.mark[u]) reach[e + 1] = 1;
            }
        }
        ok[code] = reach[n];
    });
    std::vector<uint64_t> keys;
    for (uint64_t c = 0; c < total; c++) if (ok[c]) keys.push_back(c);
    if (keys.size() > limit) {
        if (stats) stats->over_limit = true;
        return 0;
    }
    for (size_t i = 0; i < keys.size(); i += SINK_BATCH)
        sink(keys.data() + i, std::min(SINK_BATCH, keys.size() - i));
    return keys.size();
}

} // namespace

size_t find_keys(const std::string& cipher, int key_len, const Dictionary& dict, int threads,
                 size_t limit, const KeySink& sink, SearchStats* stats)
{
    const int n = (int)cipher.size();
    if (key_len <= 4)     // at most 26^4 keys
        return brute_force(cipher, key_len, dict, threads, limit, sink, stats);
    Problem P{cipher, dict, n, key_len, (n + key_len - 1) / key_len, {}};
    for (int t = 0; t < P.T; t++) P.len.push_back(std::min(key_len, n - t * key_len));
    switch (P.T) {                       // n <= 25 and key_len >= 5: 2..5 tracks
        case 2:  return column_dp<2>(P, threads, limit, sink, stats);
        case 3:  return column_dp<3>(P, threads, limit, sink, stats);
        case 4:  return column_dp<4>(P, threads, limit, sink, stats);
        default: return column_dp<5>(P, threads, limit, sink, stats);
    }
}

std::vector<uint64_t> find_keys(const std::string& cipher, int key_len, const Dictionary& dict,
                                int threads, size_t limit, SearchStats* stats)
{
    std::vector<uint64_t> keys;
    std::mutex mu;
    SearchStats st;
    find_keys(cipher, key_len, dict, threads, limit, [&](const uint64_t* k, size_t n){
        std::lock_guard<std::mutex> lock(mu);
        keys.insert(keys.end(), k, k + n);
    }, &st);
    if (stats) *stats = st;
    if (st.over_limit) keys.clear();
    return keys;
}
