#include "dp.hpp"
#include "cipher.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <type_traits>
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
// Each column is stored compactly (Layer: 8 bytes per state).
//
// Keys are then read off by walking the columns again, keeping only states that can still
// reach an accepted end and carrying, for each key prefix, the set of states it can be in;
// so every key comes out exactly once.

uint64_t pack_key(const std::string& key, int alpha)
{
    uint64_t code = 0, p = 1;
    for (char c : key) { code += (uint64_t)letter_index(c) * p; p *= alpha; }
    return code;
}

std::string unpack_key(uint64_t code, int key_len, int alpha)
{
    std::string k(key_len, 'a');
    for (int i = 0; i < key_len; i++) { k[i] = letter_char((int)(code % alpha)); code /= alpha; }
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
    int                A;     // alphabet size
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
        int x = (letter_index(P.cipher[t * P.kl + j]) - key + P.A) % P.A;
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

// One column's distinct states, stored compactly.  A state is a tuple of track states, but
// each track takes far fewer distinct values in a column than there are tuples, so a state
// is stored as one 64-bit code: the index of each track's state in that track's sorted
// table, packed `bits[t]` wide.  Codes are kept sorted, so a state's position in the column
// is a binary search.  This takes 8 bytes per state instead of 8 per track.  With 4-5
// tracks (key lengths 5-8, small columns) the codes can need more than 64 bits; they are
// 128 bits wide there.
__extension__ typedef unsigned __int128 uint128;
template <int M>
using Code = std::conditional_t<(M <= 3), uint64_t, uint128>;

template <int M>
struct Layer {
    static constexpr int BITS = sizeof(Code<M>) * 8;
    std::vector<Code<M>>                  codes;   // sorted, unique
    std::array<std::vector<uint64_t>, M>  track;   // per track: its distinct states, sorted
    std::array<int, M>                    shift{};

    // Fix the per-track tables (sorted, unique) and the bit layout.  Throws std::bad_alloc
    // (reported as out of memory) if the codes would not fit in BITS bits.
    void set_tracks(std::array<std::vector<uint64_t>, M> tables, int T) {
        track = std::move(tables);
        int pos = 0;
        for (int t = T - 1; t >= 0; t--) {
            shift[t] = pos;
            int b = 1;
            while (b < 63 && (uint64_t{1} << b) < track[t].size()) b++;
            pos += b;
        }
        if (pos > BITS) throw std::bad_alloc();
    }
    // The code of state s, or NONE if a track state is not in its table.
    static constexpr Code<M> NONE = ~Code<M>{0};
    Code<M> encode(const State<M>& s, int T) const {
        Code<M> c = 0;
        for (int t = 0; t < T; t++) {
            auto it = std::lower_bound(track[t].begin(), track[t].end(), s[t]);
            if (it == track[t].end() || *it != s[t]) return NONE;
            c |= (Code<M>)(it - track[t].begin()) << shift[t];
        }
        return c;
    }
    State<M> decode(Code<M> c, int T) const {
        State<M> s{};
        for (int t = 0; t < T; t++) {
            const int width = (t == 0 ? BITS : shift[t - 1]) - shift[t];
            const Code<M> mask = width >= BITS ? ~Code<M>{0} : (Code<M>{1} << width) - 1;
            s[t] = track[t][(size_t)((c >> shift[t]) & mask)];
        }
        return s;
    }
    // Position of state s in the column, or -1.
    long find(const State<M>& s, int T) const {
        const Code<M> c = encode(s, T);
        if (c == NONE) return -1;
        auto it = std::lower_bound(codes.begin(), codes.end(), c);
        return (it != codes.end() && *it == c) ? (long)(it - codes.begin()) : -1L;
    }
    State<M> at(size_t i, int T) const { return decode(codes[i], T); }
    size_t   size() const { return codes.size(); }
};

// Sorted, unique values, deduplicated in batches so the raw list never grows far beyond
// the number of distinct values.
template <class V>
class SortedSet {
public:
    void add(V x) { v_.push_back(x); if (v_.size() - clean_ > 8'000'000) compact(); }
    std::vector<V> take() { compact(); v_.shrink_to_fit(); return std::move(v_); }
private:
    void compact() {
        std::sort(v_.begin() + clean_, v_.end());
        v_.erase(std::unique(v_.begin() + clean_, v_.end()), v_.end());
        std::inplace_merge(v_.begin(), v_.begin() + clean_, v_.end());
        v_.erase(std::unique(v_.begin(), v_.end()), v_.end());
        clean_ = v_.size();
    }
    std::vector<V> v_;
    size_t clean_ = 0;
};

template <int M>
size_t column_dp(const Problem& P, int threads, size_t limit, const KeySink& sink, SearchStats* stats)
{
    const int T = P.T;

    // Forward: the distinct states after each column.  Each column is built in two passes
    // over the successors of the previous one: the first collects each track's distinct
    // states (the tables of the compact codes), the second the distinct codes.
    std::vector<Layer<M>> layers(1);
    {
        const auto init = initial_states<M>(P);
        std::array<std::vector<uint64_t>, M> tables;
        for (int t = 0; t < T; t++) {
            for (const auto& s : init) tables[t].push_back(s[t]);
            std::sort(tables[t].begin(), tables[t].end());
            tables[t].erase(std::unique(tables[t].begin(), tables[t].end()), tables[t].end());
        }
        layers[0].set_tracks(std::move(tables), T);
        for (const auto& s : init) layers[0].codes.push_back(layers[0].encode(s, T));
        std::sort(layers[0].codes.begin(), layers[0].codes.end());
        layers[0].codes.erase(std::unique(layers[0].codes.begin(), layers[0].codes.end()), layers[0].codes.end());
    }
    for (int j = 0; j < P.kl; j++) {
        const Layer<M>& prev = layers.back();
        auto each_successor = [&](auto&& fn) {
            for (size_t i = 0; i < prev.size(); i++) {
                const State<M> s = prev.at(i, T);
                for (int k = 0; k < P.A; k++) successors<M>(P, s, j, k, fn);
            }
        };
        Layer<M> next;
        {
            std::array<SortedSet<uint64_t>, M> per_track;
            each_successor([&](const State<M>& o){ for (int t = 0; t < T; t++) per_track[t].add(o[t]); });
            std::array<std::vector<uint64_t>, M> tables;
            for (int t = 0; t < T; t++) tables[t] = per_track[t].take();
            next.set_tracks(std::move(tables), T);
        }
        {
            SortedSet<Code<M>> codes;
            each_successor([&](const State<M>& o){ codes.add(next.encode(o, T)); });
            next.codes = codes.take();
        }
        if (stats) stats->peak_states = std::max(stats->peak_states, next.size());
        if (next.size() == 0) return 0;
        layers.push_back(std::move(next));
    }

    // Backward: which states can still reach an accepted end.
    const int L = P.kl;
    auto index = [&](int j, const State<M>& s) { return layers[j].find(s, T); };
    std::vector<std::vector<char>> live(L + 1);
    live[L].resize(layers[L].size());
    parallel_for(layers[L].size(), threads, [&](size_t i){ live[L][i] = accept<M>(P, layers[L].at(i, T)); });
    for (int j = L - 1; j >= 0; j--) {
        live[j].assign(layers[j].size(), 0);
        parallel_for(layers[j].size(), threads, [&](size_t i){
            const State<M> s = layers[j].at(i, T);
            for (int k = 0; k < P.A && !live[j][i]; k++)
                successors<M>(P, s, j, k, [&](const State<M>& o){
                    long x = index(j + 1, o);
                    if (x >= 0 && live[j + 1][x]) live[j][i] = 1;
                });
        });
    }

    // Keys: walk key prefixes carrying the set of live states each prefix can be in.
    // Split by the first key letter into A tasks; each hands its keys to the sink in batches.
    std::vector<long> start;
    for (size_t i = 0; i < layers[0].size(); i++) if (live[0][i]) start.push_back((long)i);
    if (start.empty()) return 0;
    uint64_t powA[13] = {1};
    for (int i = 1; i <= 12; i++) powA[i] = powA[i - 1] * P.A;
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
        for (int k = 0; k < P.A && !over; k++) {
            if (only >= 0 && k != only) continue;
            std::vector<long> nx;
            for (long i : set_j)
                successors<M>(P, layers[j].at(i, T), j, k, [&](const State<M>& o){
                    long x = index(j + 1, o);
                    if (x >= 0 && live[j + 1][x]) nx.push_back(x);
                });
            if (nx.empty()) continue;
            std::sort(nx.begin(), nx.end());
            nx.erase(std::unique(nx.begin(), nx.end()), nx.end());
            walk(j + 1, nx, code + (uint64_t)k * powA[j], out, -1);
        }
    };
    parallel_for(P.A, threads, [&](size_t k){
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
    const int A = d.alphabet.size();
    for (int i = 0; i < kl; i++) total *= A;
    const int n = (int)cipher.size();
    std::vector<char> ok(total, 0);
    parallel_for(total, threads, [&](size_t code){
        std::string key = unpack_key(code, kl, A), plain = vig_decrypt(cipher, key, A);
        std::vector<char> reach(n + 1, 0);
        reach[0] = 1;
        for (int i = 0; i < n; i++) {
            if (!reach[i]) continue;
            int u = 0;
            for (int e = i; e < n; e++) {
                if ((u = d.trie.ch[u][letter_index(plain[e])]) < 0) break;
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
    if (key_len <= 4)     // at most 33^4 = 1.2 million keys
        return brute_force(cipher, key_len, dict, threads, limit, sink, stats);
    Problem P{cipher, dict, n, key_len, (n + key_len - 1) / key_len, {}, dict.alphabet.size()};
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
