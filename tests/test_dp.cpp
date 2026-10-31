// Unit tests: the column DP against an independent brute force, and the index file.
// Run from the repository root (make test).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>
#include "cipher.hpp"
#include "dp.hpp"
#include "index.hpp"
#include "tables.hpp"

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

static std::string encrypt(const std::string& plain, const std::string& key)
{
    std::string c(plain.size(), 'a');
    for (size_t i = 0; i < plain.size(); i++) c[i] = vig_enc(plain[i], key[i % key.size()]);
    return c;
}

// Reference: does `text` split completely into dictionary words?  Plain DP over substrings
// with hash-set lookups, deliberately independent of the tries the search uses.
static bool splits_into_words(const Dictionary& d, const std::string& text)
{
    std::vector<char> ok(text.size() + 1, 0);
    ok[0] = 1;
    for (size_t e = 1; e <= text.size(); e++)
        for (size_t b = 0; b < e && !ok[e]; b++)
            ok[e] = ok[b] && d.words.count(text.substr(b, e - b));
    return ok[text.size()];
}

// Reference: every key of length kl whose plaintext splits into words.
static std::set<uint64_t> brute_force(const Dictionary& d, const std::string& cipher, int kl)
{
    uint64_t total = 1;
    for (int i = 0; i < kl; i++) total *= ALPHA;
    std::vector<char> hit(total, 0);
    parallel_for(total, 0 + (int)sysconf(_SC_NPROCESSORS_ONLN), [&](size_t code){
        hit[code] = splits_into_words(d, vig_decrypt(cipher, unpack_key(code, kl)));
    });
    std::set<uint64_t> keys;
    for (uint64_t c = 0; c < total; c++) if (hit[c]) keys.insert(c);
    return keys;
}

// Reference: the best score of `text` under model m by trying every split into dictionary
// words and stray letters, scoring each split word by word.  Independent of the DPs.
static double best_split(const Dictionary& d, const LanguageModel& m, const std::string& text)
{
    double best = -1e300;
    std::function<void(size_t, int, double)> go = [&](size_t pos, int prev, double acc) {
        if (pos == text.size()) { best = std::max(best, acc); return; }
        go(pos + 1, -1, acc + m.stray_logp);
        for (size_t e = pos + 1; e <= text.size(); e++) {
            const std::string w = text.substr(pos, e - pos);
            if (!d.words.count(w)) continue;
            const int v = d.trie.find(w);
            double lp = m.uni[v];
            if (m.kind == ModelKind::Bigram && prev >= 0) {
                const double* p = m.pairs.get(PairTable::key(prev, v));
                lp = p ? *p : m.backoff[prev] + m.uni[v];
            }
            go(e, m.kind == ModelKind::Bigram ? v : -1, acc + lp);
        }
    };
    go(0, -1, 0);
    return best;
}

// Score of a formatted split "[w:r]x[w:r]..." under model m, word by word.
static double score_of_split(const Dictionary& d, const LanguageModel& m, const std::string& fmt,
                             std::string* text)
{
    double acc = 0;
    int prev = -1;
    text->clear();
    for (size_t i = 0; i < fmt.size(); ) {
        if (fmt[i] != '[') { *text += fmt[i++]; acc += m.stray_logp; prev = -1; continue; }
        size_t colon = fmt.find(':', i), close = fmt.find(']', i);
        const std::string w = fmt.substr(i + 1, colon - i - 1);
        *text += w;
        const int v = d.trie.find(w);
        double lp = m.uni[v];
        if (m.kind == ModelKind::Bigram && prev >= 0) {
            const double* p = m.pairs.get(PairTable::key(prev, v));
            lp = p ? *p : m.backoff[prev] + m.uni[v];
        }
        acc += lp;
        prev = m.kind == ModelKind::Bigram ? v : -1;
        i = close + 1;
    }
    return acc;
}

int main()
{
    Dictionary dict = load_dictionary("txt/english_words.txt");
    const LanguageModel zipf = load_model(ModelKind::Zipf, dict, "txt");
    const int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);

    // ── find_keys equals brute force ─────────────────────────────────────────
    // Key length 5 uses the column DP; 8, 12, 17 and 22 letters give it 2, 3, 4 and 5
    // tracks, with words crossing track boundaries.  Key lengths 1-4 check the brute-force
    // path against the independent reference.
    struct Case { std::string plain, key; };
    const std::vector<Case> cases = {
        {"iloveyou", "bhbre"},
        {"meetmeatnoon", "emubq"},
        {"sisterlikesshopping", "somekey"},
        {"theeaglehaslandedonthe", "abcde"},
    };
    for (const auto& c : cases) {
        const std::string cipher = encrypt(c.plain, c.key);
        for (int kl = 1; kl <= 5; kl++) {
            auto got = find_keys(cipher, kl, dict, threads, SIZE_MAX);
            std::set<uint64_t> got_set(got.begin(), got.end());
            auto want = brute_force(dict, cipher, kl);
            check(got.size() == got_set.size() && got_set == want,
                  c.plain + " (" + std::to_string(cipher.size()) + " letters), key length " + std::to_string(kl) +
                  ": " + std::to_string(got.size()) + " keys, brute force " + std::to_string(want.size()));
        }
    }

    // ── language models ──────────────────────────────────────────────────────
    const LanguageModel unigram = load_model(ModelKind::Unigram, dict, "txt");
    const LanguageModel bigram  = load_model(ModelKind::Bigram, dict, "txt");
    {
        // P(w) and P(w | w1) each sum to 1 over the dictionary.
        double total = 0;
        for (const auto& w : dict.words) total += std::exp(unigram.uni[dict.trie.find(w)]);
        check(std::fabs(total - 1) < 1e-9, "unigram: P(w) sums to 1");
        for (const char* w1 : {"the", "i", "of", "miss", "zebra"}) {
            const int u = dict.trie.find(w1);
            double sum = 0;
            for (const auto& w : dict.words) {
                const int v = dict.trie.find(w);
                const double* p = bigram.pairs.get(PairTable::key(u, v));
                sum += std::exp(p ? *p : bigram.backoff[u] + bigram.uni[v]);
            }
            check(std::fabs(sum - 1) < 1e-9, std::string("bigram: P(w | ") + w1 + ") sums to 1");
        }
        // The DPs find the best split, and the split they report has that score.
        const std::vector<std::string> texts = {"imissyousomuch", "meetmeatnoon", "ofareaswithout",
                                                "theqcatxsat", "helloworld", "xq", "a"};
        for (const LanguageModel* m : {&zipf, &unigram, &bigram})
            for (const auto& t : texts) {
                std::string fmt, back;
                const double got = segment(dict, *m, t, &fmt);
                const double ref = best_split(dict, *m, t);
                const double re  = score_of_split(dict, *m, fmt, &back);
                check(std::fabs(got - ref) < 1e-9 && std::fabs(got - re) < 1e-9 && back == t,
                      m->name + ": best split of " + t + " " + fmt);
            }
        // Grammatical word order beats the same words shuffled only with word pairs.
        const double ok = segment(dict, bigram, "iwillbetheresoon"), bad = segment(dict, bigram, "therebesoonwilli");
        const double uok = segment(dict, unigram, "iwillbetheresoon"), ubad = segment(dict, unigram, "therebesoonwilli");
        check(ok > bad + 5 && std::fabs(uok - ubad) < 1e-9, "bigram: word order matters, unigram: it does not");
        // The zipf model is the original scorer: rank-based log-probabilities, stray -15.
        check(zipf.uni == dict.node_logp && zipf.stray_logp == -15 && zipf.fingerprint == "-",
              "zipf: the original rank-based model");
    }

    // ── index: round trip, order, rank_of ────────────────────────────────────
    char tmpl[] = "/tmp/vigidx_test_XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    const std::string path = tmpl;
    {
        const std::string plain = "helloworld", cipher = encrypt(plain, "code");
        IndexOptions opt;
        opt.max_key_length = 6;
        auto sum = build_index(cipher, dict, zipf, opt, path);
        size_t per_kl = 0;
        for (auto [kl, n] : sum.per_key_length) per_kl += n;
        check(sum.count > 0 && per_kl == sum.count, "index: per-key-length counts add up");

        IndexReader idx(path, dict, zipf);
        check(idx.cipher() == cipher && idx.summary().count == sum.count, "index: header read back");
        auto all = idx.range(1, idx.summary().count);
        bool sorted = all.size() == sum.count;
        std::set<std::string> plains;
        for (size_t i = 0; i < all.size(); i++) {
            sorted = sorted && all[i].rank == i + 1 && (i == 0 || all[i - 1].score >= all[i].score);
            plains.insert(all[i].plain);
        }
        check(sorted, "index: ranks 1..count in descending score order");
        check(plains.size() == all.size(), "index: every plaintext appears once");
        size_t r = idx.rank_of(plain);
        check(r > 0 && idx.range(r, r)[0].plain == plain && idx.range(r, r)[0].key == "code",
              "index: rank_of finds the true plaintext (rank " + std::to_string(r) + ")");
        check(idx.rank_of("zzzzzzzzzz") == 0, "index: rank_of reports a missing plaintext");
        check(idx.range(sum.count + 1, sum.count + 5).empty(), "index: range past the end is empty");
    }
    {
        // rerank: same candidates in bigram order; back to zipf restores the file exactly.
        auto read_all = [](const std::string& p) {
            FILE* f = std::fopen(p.c_str(), "rb");
            std::string s;
            char buf[1 << 16];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, got);
            std::fclose(f);
            return s;
        };
        const std::string original = read_all(path), other = path + ".bigram";
        rerank_index(path, other, dict, bigram, threads);
        check(index_model_name(other) == "bigram" && index_model_name(path) == "zipf",
              "rerank: model recorded in the header");
        IndexReader zi(path, dict, zipf), bi(other, dict, bigram);
        auto za = zi.range(1, zi.summary().count), ba = bi.range(1, bi.summary().count);
        std::set<std::string> zk, bk;
        bool sorted = true;
        for (const auto& c : za) zk.insert(c.key);
        for (size_t i = 0; i < ba.size(); i++) {
            bk.insert(ba[i].key);
            sorted = sorted && (i == 0 || ba[i - 1].score >= ba[i].score);
        }
        check(zk == bk && sorted && bi.summary().count == zi.summary().count,
              "rerank: same candidates, descending bigram score");
        check(bi.rank_of("helloworld") > 0 && bi.rank_of("helloworld") <= zi.rank_of("helloworld"),
              "rerank: helloworld ranks at least as high by bigram (" + std::to_string(bi.rank_of("helloworld")) +
              " vs " + std::to_string(zi.rank_of("helloworld")) + ")");
        bool threw = false;
        try { IndexReader wrong(other, dict, zipf); } catch (const std::exception&) { threw = true; }
        check(threw, "rerank: reading with another model is rejected");
        rerank_index(other, other, dict, zipf, threads);     // in place
        const std::string back = read_all(other);
        check(back.size() == original.size() &&
              back.compare(INDEX_HEADER_BYTES, std::string::npos, original, INDEX_HEADER_BYTES, std::string::npos) == 0,
              "rerank: back to zipf in place gives the original order");
        unlink(other.c_str());
    }
    {
        // A ciphertext that repeats with period 3: key length 3 carries no information.
        IndexOptions opt;
        opt.max_key_length = 4;
        auto sum = build_index("abcabcabcab", dict, zipf, opt, path);
        bool skipped = std::any_of(sum.notes.begin(), sum.notes.end(),
                                   [](const std::string& s){ return s.find("key length 3 skipped") == 0; });
        bool listed = std::any_of(sum.per_key_length.begin(), sum.per_key_length.end(),
                                  [](auto p){ return p.first == 3; });
        check(skipped && !listed, "index: repeating ciphertext skips that key length");
    }
    {
        bool threw = false;
        try { IndexReader bad("txt/english_words.txt", dict, zipf); } catch (const std::exception&) { threw = true; }
        check(threw, "index: a non-index file is rejected");
    }
    unlink(path.c_str());

    std::printf("\nunit tests: %d failed\n", failures);
    return failures ? 1 : 0;
}
