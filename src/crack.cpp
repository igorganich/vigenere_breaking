#include "crack.hpp"
#include "cipher.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <functional>
#include <limits>
#include <unordered_map>
#include <climits>
#include <cstdint>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>
#include <random>
#include <numeric>

// ── Tuning constants ──────────────────────────────────────────────────────────

static constexpr int    MAX_KEY_LEN  = 12;
static constexpr double MIN_COVERAGE = 0.90; // drop candidates below this word coverage

// Calibration: per bucket, time up to CALIB_SAMPLES random first words, spending
// at most CALIB_BUCKET_SECONDS; all buckets together get at most CALIB_SHARE of
// the time limit (or CALIB_MAX_SECONDS without one).
static constexpr int    CALIB_SAMPLES        = 16;
static constexpr double CALIB_BUCKET_SECONDS = 0.05;
static constexpr double CALIB_SHARE          = 0.10;
static constexpr double CALIB_MAX_SECONDS    = 20.0;

// Parallel search: a bucket with fewer than SPLIT_WORDS_PER_THREAD × threads
// first words is split further, by chunks of SECOND_WORD_CHUNK second words.
static constexpr int    SPLIT_WORDS_PER_THREAD = 4;
static constexpr int    SECOND_WORD_CHUNK      = 256;

// ── Internal raw candidate (plain + key, before segmentation) ─────────────────

// ── Candidate pool: the best `cap` distinct plaintexts seen so far ───────────
// Memory stays bounded however long the search runs: once the pool holds 2×cap
// entries it is pruned back to the best cap, and from then on anything scoring
// below the cap-th best is rejected on arrival.  The same plaintext reached with
// several keys (e.g. "abc" and its repeat "abcabc") is kept once, with the
// higher score, i.e. the shorter key.

struct CandidatePool {
    size_t cap;    // 0 = unlimited
    double floor = -std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, CrackCandidate> by_plain;

    explicit CandidatePool(int cap_) : cap(cap_ > 0 ? (size_t)cap_ : 0) {}

    void add(std::string plain, std::string key, WordSegment seg) {
        // A key is a priori one of 26^kl strings, so a plaintext reachable with a
        // short key is stronger evidence than the same-quality text from a long key.
        double score = seg.lm - (double)key.size() * std::log((double)ALPHA);
        if (score < floor) return;
        auto it = by_plain.find(plain);
        if (it != by_plain.end()) {
            if (score > it->second.score) it->second = {plain, std::move(key), 0, std::move(seg), score};
            return;
        }
        CrackCandidate c{plain, std::move(key), 0, std::move(seg), score};
        by_plain.emplace(std::move(plain), std::move(c));
        if (cap && by_plain.size() >= 2 * cap) prune();
    }

    void prune() {
        std::vector<double> scores;
        scores.reserve(by_plain.size());
        for (auto& [p, c] : by_plain) scores.push_back(c.score);
        std::nth_element(scores.begin(), scores.begin() + (cap - 1), scores.end(), std::greater<>());
        floor = scores[cap - 1];
        for (auto it = by_plain.begin(); it != by_plain.end(); )
            it = it->second.score < floor ? by_plain.erase(it) : std::next(it);
    }

    // Fold another pool (e.g. one thread's) into this one.
    void merge(CandidatePool&& other) {
        for (auto& [p, c] : other.by_plain) add(p, std::move(c.key), std::move(c.segment));
        other.by_plain.clear();
    }

    // Best candidates, most probable first.
    std::vector<CrackCandidate> take(const Tables& t) {
        static constexpr char NOOP_KEY[] = "a";
        std::vector<CrackCandidate> out;
        out.reserve(by_plain.size());
        for (auto& [p, c] : by_plain) out.push_back(std::move(c));
        by_plain.clear();
        std::sort(out.begin(), out.end(),
                  [](const CrackCandidate& a, const CrackCandidate& b){
                      return a.score != b.score ? a.score > b.score : a.plain < b.plain;
                  });
        if (cap && out.size() > cap) out.resize(cap);
        for (auto& c : out)
            c.fitness = fitness(c.plain.data(), (int)c.plain.size(), NOOP_KEY, 1, t.quad);
        return out;
    }
};

// ── Dictionary trie ───────────────────────────────────────────────────────────
// Children are visited in order of the most common word in their subtree, so
// frequent words tend to be tried first.  by_len keeps the words of each length
// in exact frequency order for the root, where the result cap applies.

struct Trie {
    struct Node {
        int     child[ALPHA];
        int     best_rank = INT_MAX; // most common (lowest) word rank in subtree
        bool    end = false;         // a dictionary word ends here
        uint8_t order[ALPHA];        // existing child letters, by best_rank
        int     n_children = 0;
        Node() { std::fill(std::begin(child), std::end(child), -1); }
    };
    std::vector<Node> nodes;
    std::vector<std::vector<std::string>> by_len;

    explicit Trie(const Tables& t) : nodes(1), by_len(t.max_word_len + 1) {
        for (const auto& w : t.dict_full) {
            by_len[w.size()].push_back(w);
            int rank = t.word_rank.at(w), cur = 0;
            nodes[0].best_rank = std::min(nodes[0].best_rank, rank);
            for (char c : w) {
                int ci = c - 'a';
                if (nodes[cur].child[ci] == -1) {
                    nodes[cur].child[ci] = (int)nodes.size();
                    nodes.emplace_back();
                }
                cur = nodes[cur].child[ci];
                nodes[cur].best_rank = std::min(nodes[cur].best_rank, rank);
            }
            nodes[cur].end = true;
        }
        for (auto& nd : nodes) {
            for (int ci = 0; ci < ALPHA; ci++)
                if (nd.child[ci] != -1) nd.order[nd.n_children++] = (uint8_t)ci;
            std::sort(nd.order, nd.order + nd.n_children, [&](uint8_t a, uint8_t b){
                return nodes[nd.child[a]].best_rank < nodes[nd.child[b]].best_rank;
            });
        }
    }
};

// ── Wall-clock deadline, polled cheaply from the search ──────────────────────

struct Deadline {
    using Clock = std::chrono::steady_clock;
    bool              enabled = false;
    Clock::time_point at;
    bool              hit   = false;
    unsigned          ticks = 0;

    explicit Deadline(double seconds) { reset(seconds); }
    Deadline(bool enabled_, Clock::time_point at_) : enabled(enabled_), at(at_) {}

    void reset(double seconds) {
        enabled = seconds > 0; hit = false; ticks = 0;
        if (enabled)
            at = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(seconds));
    }
    bool check() {
        if (enabled && !hit && (++ticks & 1023) == 0 && Clock::now() >= at) hit = true;
        return hit;
    }
};

// ── Per-bucket caps, shared by every thread working on the bucket ───────────
// Counters may dip below zero under contention; anything ≤ 0 means exhausted.

struct BucketLimits {
    bool capped_results = false, capped_nodes = false;
    std::atomic<long long> results{0}, partials{0}, nodes{0};

    void init(const WcsParams& p) {
        capped_results = p.results > 0;
        capped_nodes   = p.budget  > 0;
        results = partials = p.results;
        nodes = p.budget;
    }
    bool results_out() const { return capped_results && results.load(std::memory_order_relaxed) <= 0; }
    bool nodes_out()   const { return capped_nodes   && nodes.load(std::memory_order_relaxed)   <= 0; }
    bool exhausted()   const { return results_out() || nodes_out(); }
};

// ── Word cover search: recursive CSP word placement ──────────────────────────
// Recursively places dictionary words starting at the leftmost uncovered
// cipher position, propagating key constraints at each step.  Multiple short
// words collectively pin down the full key, so no single word has to span it.
//
// Words are found by walking the trie along the ciphertext: where the key slot
// is already fixed the plaintext letter is known and only one child is
// followed; where it is free, every child is tried and fixes that slot.
// Contradictions are therefore never generated rather than checked per word.
//
// Once every key slot is fixed, the rest of the plaintext is fully determined,
// so the search stops branching: it decrypts once and accepts the candidate if
// its word coverage passes the same threshold as the final output filter.  Each
// key is counted once, however many word splits lead to it.  Full coverings and
// near-misses (90–99% coverage) are capped separately; only the former stop
// the search.
//
// The search is split into one bucket per first-word length.  Each bucket tries
// its first words in frequency order from by_len (not by trie walk, whose
// depth-first order would exhaust the cap on one initial letter) and has its own
// result cap and node budget, so buckets never crowd each other out and the
// order they run in only changes when results arrive, not which.

struct WordCoverSearch {
    const std::string& cipher;
    const int          n, kl;
    const Tables&      t;
    const Trie&        trie;
    Deadline&          deadline;

    std::vector<int> key_buf;   // -1 = slot not fixed yet
    int              n_fixed = 0;
    CandidatePool&   pool;
    BucketLimits&    lim;
    long long        local_nodes = 0;          // placements not yet charged to lim.nodes
    std::unordered_set<std::string> seen_keys; // only with caps on (unbounded otherwise)

    WordCoverSearch(const std::string& c, int kl_, const Tables& t_, const Trie& tr,
                    Deadline& dl, CandidatePool& pl, BucketLimits& lm)
        : cipher(c), n((int)c.size()), kl(kl_), t(t_), trie(tr), deadline(dl), key_buf(kl_, -1),
          pool(pl), lim(lm) {}

    bool stopped() const { return deadline.hit || lim.exhausted(); }

    void accept_full_key() {
        std::string key(kl, 'a');
        for (int i = 0; i < kl; i++) key[i] = 'a' + key_buf[i];
        // With caps on, a key is counted once however many word splits reach it.
        // Uncapped, nothing is counted and the pool deduplicates on its own.
        if (lim.capped_results && !seen_keys.insert(key).second) return;
        std::string plain = vig_decrypt(cipher, key);
        WordSegment seg = word_segment(plain, t);
        const double cov = seg.coverage;
        if (cov < MIN_COVERAGE) return;
        // Near-misses (< 100% coverage) have their own cap and never stop the
        // search, so they cannot crowd out the rarer true first word.
        if (lim.capped_results) {
            if (cov < 1.0) { if (lim.partials.fetch_sub(1) <= 0) return; }
            else           lim.results.fetch_sub(1);
        }
        pool.add(std::move(plain), std::move(key), std::move(seg));
    }

    // Fix the key slots for `word` at position 0 (key empty, so only self-wrap
    // can conflict), run then(L) if consistent, and clear the key again.
    template <class Then>
    void with_first(const std::string& word, Then then) {
        const int L = (int)word.size();
        bool ok = true;
        for (int j = 0; j < L && ok; j++) {
            int kval = (cipher[j] - word[j] + ALPHA) % ALPHA;
            int& slot = key_buf[j % kl];
            if (slot == -1) { slot = kval; n_fixed++; }
            else ok = (slot == kval);
        }
        if (ok) then(L);
        std::fill(key_buf.begin(), key_buf.end(), -1);
        n_fixed = 0;
    }
    void place_first(const std::string& word) {
        with_first(word, [&](int L){ place(L, 1, INT_MAX); });
    }

    // Place a specific `word` at p if it fits and is consistent, then recurse.
    void place_word_at(int p, const std::string& word) {
        const int L = (int)word.size();
        if (p + L > n) return;
        int set_kpos[MAX_KEY_LEN], n_set = 0;
        bool ok = true;
        for (int j = 0; j < L && ok; j++) {
            int kpos = (p + j) % kl, kval = (cipher[p + j] - word[j] + ALPHA) % ALPHA;
            if (key_buf[kpos] == -1) { key_buf[kpos] = kval; set_kpos[n_set++] = kpos; n_fixed++; }
            else ok = (key_buf[kpos] == kval);
        }
        if (ok) place(p + L, 1, INT_MAX);
        for (int i = 0; i < n_set; i++) key_buf[set_kpos[i]] = -1;
        n_fixed -= n_set;
    }

    // Place a word starting at p (everything before p is covered), with length
    // in [min_L, max_L].
    void place(int p, int min_L, int max_L) {
        if (stopped() || deadline.check()) return;
        if (lim.capped_nodes && ++local_nodes == 256) {
            lim.nodes.fetch_sub(local_nodes, std::memory_order_relaxed);
            local_nodes = 0;
        }
        if (n_fixed == kl) { accept_full_key(); return; }
        walk(0, p, 0, min_L, std::min(max_L, n - p));
    }

    // At trie node `node` having matched d letters of a word starting at p.
    void walk(int node, int p, int d, int min_L, int max_L) {
        if (stopped()) return;
        const auto& nd = trie.nodes[node];
        if (nd.end && d >= min_L) place(p + d, 1, INT_MAX);
        if (d == max_L) return;

        const int pos = p + d, kpos = pos % kl, c = cipher[pos] - 'a';
        if (key_buf[kpos] != -1) {
            int child = nd.child[(c - key_buf[kpos] + ALPHA) % ALPHA];
            if (child != -1) walk(child, p, d + 1, min_L, max_L);
            return;
        }
        n_fixed++;
        for (int i = 0; i < nd.n_children && !stopped(); i++) {
            int letter = nd.order[i];
            key_buf[kpos] = (c - letter + ALPHA) % ALPHA;
            walk(nd.child[letter], p, d + 1, min_L, max_L);
        }
        key_buf[kpos] = -1;
        n_fixed--;
    }
};

// ── Calibration: estimate each bucket's full cost by sampling ────────────────
// Knuth-style estimate: the cost of searching every first word of a bucket is
// N × the mean cost of a uniform random sample of them, timed with caps off.
// First words are searched in frequency order, so the time spent on a partly
// searched bucket says little about its remaining (rarer) words; the sample does.

static void calibrate(const std::string& cipher, const Tables& t, const Trie& trie,
                      const WcsParams& p, BucketStats& b, double allowance, std::mt19937& rng)
{
    using Clock = std::chrono::steady_clock;
    const auto& words = trie.by_len[b.L];
    b.n_words = (int)words.size();
    if (b.n_words == 0) { b.est_full = 0; return; }
    if (allowance <= 0) return;

    std::vector<int> idx(b.n_words);
    std::iota(idx.begin(), idx.end(), 0);
    if (b.n_words > CALIB_SAMPLES) {
        std::vector<int> pick;
        std::sample(idx.begin(), idx.end(), std::back_inserter(pick), CALIB_SAMPLES, rng);
        idx.swap(pick);
    }

    Deadline        dl(0);
    CandidatePool   scratch(p.top);
    BucketLimits    no_caps;                    // caps off
    WordCoverSearch probe(cipher, b.kl, t, trie, dl, scratch, no_caps);
    double spent = 0;
    for (int i : idx) {
        if (allowance - spent <= 0) { b.est_lower_bound = true; break; }
        dl.reset(allowance - spent);
        auto t0 = Clock::now();
        probe.place_first(words[i]);
        spent += std::chrono::duration<double>(Clock::now() - t0).count();
        b.samples++;
        if (dl.hit) { b.est_lower_bound = true; break; }
    }
    if (b.samples > 0) b.est_full = spent / b.samples * b.n_words;
}

// ── Top-level: search every key length, deduplicate, segment, rank ───────────
//
// Buckets run as an anytime search, cheapest and most constraining first:
//   stage 0:  for every kl, first words of length ≥ kl−1, longest first
//             (a first word that long fixes nearly the whole key, so this is fast);
//   stage s:  for every kl, first words of length kl−1−s, down to min_word.
// Shorter first words fix fewer key slots and branch far more, so each stage is
// costlier than the last.  When the time limit expires the search stops where it
// is and returns everything found so far.  Buckets are spread over threads in
// that same order.

std::vector<CrackCandidate> crack(const std::string& cipher, const Tables& t,
                                  WcsParams wcs, CrackStats* stats)
{
    using Clock = std::chrono::steady_clock;
    const auto start  = Clock::now();
    auto since = [](Clock::time_point t0){
        return std::chrono::duration<double>(Clock::now() - t0).count();
    };

    const int  clen   = (int)cipher.size();
    const int  max_kl = std::min(MAX_KEY_LEN, clen - 1);
    const Trie trie(t);

    // ── Plan the buckets in run order ─────────────────────────────────────────
    auto floor_len  = [&](int kl){ return std::max(2, kl - 1); };
    auto lowest_len = [&](int kl){ return std::max(1, std::min(kl - 1, wcs.min_word)); };
    const int max_first = std::min(t.max_word_len, clen);

    std::vector<BucketStats> plan;
    for (int kl = 1; kl <= max_kl; kl++)
        for (int L = max_first; L >= floor_len(kl); L--)
            plan.push_back({kl, L, 0});
    for (int stage = 1; ; stage++) {
        bool any = false;
        for (int kl = 1; kl <= max_kl; kl++) {
            int L = floor_len(kl) - stage;
            if (L >= lowest_len(kl)) { plan.push_back({kl, L, stage}); any = true; }
        }
        if (!any) break;
    }

    for (auto& b : plan) b.n_words = (int)trie.by_len[b.L].size();

    // ── Worker threads take buckets in plan order ────────────────────────────
    // Buckets are independent, so each thread keeps its own search state and
    // candidate pool; the pools are merged at the end, giving the same result
    // as a single thread (the best `top` distinct plaintexts overall).
    const int n_threads = std::max(1, std::min<int>(
        wcs.threads > 0 ? wcs.threads : (int)std::thread::hardware_concurrency(),
        (int)plan.size()));
    auto parallel = [&](auto&& work) {   // work(bucket index, thread index)
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (int tid = 0; tid < n_threads; tid++)
            pool.emplace_back([&, tid]{
                for (size_t i; (i = next++) < plan.size(); ) if (!work(i, tid)) break;
            });
        for (auto& th : pool) th.join();
    };

    // ── Calibrate every bucket ────────────────────────────────────────────────
    {
        const double total = wcs.time_limit > 0 ? CALIB_SHARE * wcs.time_limit : CALIB_MAX_SECONDS;
        const double per_bucket = std::min(CALIB_BUCKET_SECONDS, total * n_threads / plan.size());
        parallel([&](size_t i, int){
            std::mt19937 rng(12345 + (unsigned)i);
            calibrate(cipher, t, trie, wcs, plan[i], per_bucket, rng);
            return true;
        });
    }
    const double calibration = since(start);

    // ── Search ────────────────────────────────────────────────────────────────
    // All threads work on the current bucket together, each taking its next
    // unit, and move on once its units are handed out, so buckets still finish
    // in plan order.  A unit is one first word; in buckets with very few first
    // words (e.g. "a"/"i") one word can be most of the search, so there a unit
    // is one first word × one chunk of second words.
    struct BucketWork {
        BucketLimits                        lim;
        int                                 chunks_per_word = 1;
        long long                           units = 0;
        std::atomic<long long>              next{0};
        std::unique_ptr<std::atomic<int>[]> chunks_left;   // per first word
        std::atomic<long long>              work_ns{0};
        std::atomic<bool>                   started{false};
    };
    const int n_second_chunks =
        ((int)t.dict_full.size() + SECOND_WORD_CHUNK - 1) / SECOND_WORD_CHUNK;
    std::vector<std::unique_ptr<BucketWork>> works;
    for (const auto& b : plan) {
        auto w = std::make_unique<BucketWork>();
        w->lim.init(wcs);
        if (n_threads > 1 && b.n_words < SPLIT_WORDS_PER_THREAD * n_threads)
            w->chunks_per_word = n_second_chunks;
        w->units = (long long)b.n_words * w->chunks_per_word;
        w->chunks_left = std::make_unique<std::atomic<int>[]>(b.n_words);
        for (int i = 0; i < b.n_words; i++) w->chunks_left[i] = w->chunks_per_word;
        works.push_back(std::move(w));
    }

    const bool limited = wcs.time_limit > 0;
    const auto at = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(std::max(0.0, wcs.time_limit - calibration)));
    std::vector<CandidatePool> pools(n_threads, CandidatePool(wcs.top));
    std::vector<Deadline>      deadlines(n_threads, Deadline(limited, at));
    std::atomic<size_t>        current{0};
    std::atomic<int>           running{n_threads};

    std::vector<std::thread> threads;
    for (int tid = 0; tid < n_threads; tid++) threads.emplace_back([&, tid]{
        Deadline& dl = deadlines[tid];
        std::unique_ptr<WordCoverSearch> s;
        size_t s_bucket = SIZE_MAX;
        for (;;) {
            size_t bi = current.load();
            if (bi >= plan.size()) break;
            if (dl.hit || (limited && Clock::now() >= at)) { dl.hit = true; break; }
            BucketWork& w = *works[bi];
            long long u = w.lim.exhausted() ? w.units : w.next.fetch_add(1);
            if (u >= w.units) { current.compare_exchange_strong(bi, bi + 1); continue; }
            w.started = true;
            if (s_bucket != bi) {
                s = std::make_unique<WordCoverSearch>(cipher, plan[bi].kl, t, trie, dl,
                                                      pools[tid], w.lim);
                s_bucket = bi;
            }
            const int   word  = (int)(u / w.chunks_per_word);
            const int   chunk = (int)(u % w.chunks_per_word);
            const auto& first = trie.by_len[plan[bi].L][word];
            auto t0 = Clock::now();
            if (w.chunks_per_word == 1) {
                s->place_first(first);
            } else {
                s->with_first(first, [&](int L1){
                    if (s->n_fixed == s->kl) { if (chunk == 0) s->place(L1, 1, INT_MAX); return; }
                    const size_t lo = (size_t)chunk * SECOND_WORD_CHUNK;
                    const size_t hi = std::min(lo + SECOND_WORD_CHUNK, t.dict_full.size());
                    for (size_t k = lo; k < hi && !s->stopped(); k++)
                        s->place_word_at(L1, t.dict_full[k]);
                });
            }
            w.work_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - t0).count();
            if (!s->stopped()) w.chunks_left[word]--;   // else possibly cut short
        }
        running--;
    });

    // Progress line on stderr every wcs.progress seconds while threads run.
    if (wcs.progress > 0) {
        auto last = Clock::now();
        while (running > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (since(last) < wcs.progress) continue;
            last = Clock::now();
            const size_t bi = std::min(current.load(), plan.size() - 1);
            const BucketWork& w = *works[bi];
            long long cpu_ns = 0;
            for (const auto& x : works) cpu_ns += x->work_ns;
            const long long handed = std::min(w.next.load(), w.units);
            std::fprintf(stderr, "[progress] %.0fs: bucket %zu/%zu (kl %d, first word %d letters,"
                                 " stage %d): %lld/%lld units handed out; cpu %.0fs\n",
                         since(start), bi + 1, plan.size(), plan[bi].kl, plan[bi].L, plan[bi].stage,
                         handed, w.units, cpu_ns / 1e9);
        }
    }
    for (auto& th : threads) th.join();
    const bool timed_out = std::any_of(deadlines.begin(), deadlines.end(),
                                       [](const Deadline& d){ return d.hit; });

    using St = BucketStats::Status;
    for (size_t bi = 0; bi < plan.size(); bi++) {
        BucketStats& b = plan[bi];
        BucketWork&  w = *works[bi];
        b.seconds = w.work_ns / 1e9;
        const auto& words = trie.by_len[b.L];
        double mass_all = 0, mass_tried = 0;
        b.tried = 0;
        for (int i = 0; i < b.n_words; i++) {
            double m = 1.0 / t.word_rank.at(words[i]);
            mass_all += m;
            if (w.chunks_left[i] <= 0) { b.tried++; mass_tried += m; }
        }
        b.mass_tried = mass_all > 0 ? mass_tried / mass_all : 1;
        b.status = b.tried == b.n_words ? St::Complete
                 : !w.started           ? St::NotRun
                 : w.lim.nodes_out()    ? St::Budget
                 : w.lim.results_out()  ? St::ResultCap
                 :                        St::TimeLimit;
    }

    CandidatePool pool(wcs.top);
    for (auto& p : pools) pool.merge(std::move(p));

    if (stats) {
        stats->timed_out   = timed_out;
        stats->threads     = n_threads;
        stats->calibration = calibration;
        stats->search      = since(start) - calibration;
        stats->buckets     = plan;
    }

    return pool.take(t);
}
