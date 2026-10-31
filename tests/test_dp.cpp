// Unit tests: the column DP against an independent brute force, and the index file.
// Run from the repository root (make test).

#include <algorithm>
#include <cstdio>
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

int main()
{
    Dictionary dict = load_dictionary("txt/english_words.txt");
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

    // ── index: round trip, order, rank_of ────────────────────────────────────
    char tmpl[] = "/tmp/vigidx_test_XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    const std::string path = tmpl;
    {
        const std::string plain = "helloworld", cipher = encrypt(plain, "code");
        IndexOptions opt;
        opt.max_key_length = 6;
        auto sum = build_index(cipher, dict, opt, path);
        size_t per_kl = 0;
        for (auto [kl, n] : sum.per_key_length) per_kl += n;
        check(sum.count > 0 && per_kl == sum.count, "index: per-key-length counts add up");

        IndexReader idx(path, dict);
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
        // A ciphertext that repeats with period 3: key length 3 carries no information.
        IndexOptions opt;
        opt.max_key_length = 4;
        auto sum = build_index("abcabcabcab", dict, opt, path);
        bool skipped = std::any_of(sum.notes.begin(), sum.notes.end(),
                                   [](const std::string& s){ return s.find("key length 3 skipped") == 0; });
        bool listed = std::any_of(sum.per_key_length.begin(), sum.per_key_length.end(),
                                  [](auto p){ return p.first == 3; });
        check(skipped && !listed, "index: repeating ciphertext skips that key length");
    }
    {
        bool threw = false;
        try { IndexReader bad("txt/english_words.txt", dict); } catch (const std::exception&) { threw = true; }
        check(threw, "index: a non-index file is rejected");
    }
    unlink(path.c_str());

    std::printf("\nunit tests: %d failed\n", failures);
    return failures ? 1 : 0;
}
