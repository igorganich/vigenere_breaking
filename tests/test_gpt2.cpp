// Unit tests of the GPT-2 implementation against the reference (PyTorch, Hugging Face
// transformers).  Needs models/gpt2 (tools/get_gpt2.sh); skipped without it.
// Run from the repository root (make test).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>
#include "gpt2.hpp"

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

// Token ids and total log P(text) (nats, <|endoftext|> as the first context) computed with
// transformers' GPT2LMHeadModel in float32, openai-community/gpt2 at revision 607a30d.
struct Reference { std::string text; std::vector<int> ids; double logprob; };
static const std::vector<Reference> REFERENCE = {
    {"I am told to go and see it by four.", {40, 716, 1297, 284, 467, 290, 766, 340, 416, 1440, 13}, -48.703050},
    {"A jury us coupe frog the autumn.", {32, 9002, 514, 2284, 431, 21264, 262, 23608, 13}, -82.112824},
    {"Money talks.", {26788, 6130, 13}, -17.284057},
    {"The coffee is too hot.", {464, 6891, 318, 1165, 3024, 13}, -27.185964},
    {"Take the last train.", {12322, 262, 938, 4512, 13}, -28.871804},
    {"Gay cops with old enemies ship.", {41787, 14073, 351, 1468, 5775, 4074, 13}, -57.459473},
    {"Checktheseconddrawer.", {9787, 1169, 12227, 19334, 263, 13}, -39.796507},
};

int main()
{
    const std::string dir = "models/gpt2";
    if (!Gpt2::available(dir)) {
        std::printf("SKIP  GPT-2 tests: %s not found (run tools/get_gpt2.sh)\n\ngpt2 tests: skipped\n", dir.c_str());
        return 0;
    }
    const int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    Gpt2 lm(dir);

    std::vector<std::string> texts;
    for (const auto& r : REFERENCE) {
        texts.push_back(r.text);
        check(lm.encode(r.text) == r.ids, "tokenizer: \"" + r.text + "\"");
    }
    check(lm.encode("a  b   c d.") == std::vector<int>({64, 220, 275, 220, 220, 269, 288, 13}),
          "tokenizer: runs of spaces split as GPT-2 does");
    check(lm.encode("don't stop") == std::vector<int>({9099, 470, 2245}), "tokenizer: contraction");

    // Log-probabilities match the reference.  Float32 sums in a different order differ in
    // the last bits; 1e-3 nats over a sentence is far below any gap that matters for ranking.
    auto t0 = std::chrono::steady_clock::now();
    const auto lp = lm.logprob(texts, threads);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t i = 0; i < texts.size(); i++) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " %.6f (reference %.6f)", lp[i], REFERENCE[i].logprob);
        check(std::fabs(lp[i] - REFERENCE[i].logprob) < 1e-3, "log P(\"" + texts[i] + "\") =" + buf);
    }

    // The same numbers, bit for bit, with one thread and alone in a batch.
    const auto one_thread = lm.logprob(texts, 1);
    check(one_thread == lp, "determinism: 1 thread gives the same values bit for bit");
    bool alone = true;
    for (size_t i = 0; i < texts.size(); i++) alone = alone && lm.logprob({texts[i]}, threads)[0] == lp[i];
    check(alone, "determinism: a text scored alone gives the same value as in a batch");

    // Batches split at about 1024 positions: many texts must still give the same values.
    std::vector<std::string> many;
    for (int k = 0; k < 40; k++) for (const auto& t : texts) many.push_back(t);
    const auto lp_many = lm.logprob(many, threads);
    bool same = true;
    for (size_t k = 0; k < many.size(); k++) same = same && lp_many[k] == lp[k % texts.size()];
    check(same, "batching: 280 texts over several batches give the same values");

    // The real use: the true stress-case sentence beats the trigram's first choice by a margin.
    check(lp[0] - lp[1] > 20, "ranking: \"I am told to go and see it by four.\" well above \"A jury us coupe frog the autumn.\"");

    std::printf("\n%zu texts scored in %.2fs\ngpt2 tests: %d failed\n", texts.size(), secs, failures);
    return failures ? 1 : 0;
}
