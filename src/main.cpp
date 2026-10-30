#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <iomanip>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <climits>
#include "tables.hpp"
#include "cipher.hpp"
#include "crack.hpp"

// Resolve data file paths relative to the binary, so the tool works regardless
// of the working directory (e.g. when invoked from tests/ or any other path).
static std::string exe_dir()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    auto slash = p.rfind('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

static const std::string BASE        = exe_dir() + "/..";
static const std::string QUAD_FILE_S = BASE + "/txt/english_quadgrams.txt";
static const std::string DICT_FILE_S = BASE + "/txt/english_words.txt";

static std::string read_file(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << '\n'; std::exit(1); }
    return {std::istreambuf_iterator<char>(f), {}};
}

static std::string to_lower_alpha(const std::string& s)
{
    std::string out; out.reserve(s.size());
    for (unsigned char c : s)
        if (std::isalpha(c)) out += (char)std::tolower(c);
    return out;
}

static void cmd_encrypt(const char* path)
{
    std::string plain = to_lower_alpha(read_file(path));
    if (plain.empty()) { std::cerr << "empty plaintext\n"; std::exit(1); }
    std::cerr << "key: ";
    std::string key; std::cin >> key;
    key = to_lower_alpha(key);
    if (key.empty()) { std::cerr << "invalid key\n"; std::exit(1); }
    for (int i = 0; i < (int)plain.size(); i++)
        std::cout << vig_enc(plain[i], key[i % key.size()]);
    std::cout << '\n';
}

enum class Report { None, Summary, Buckets, All };

// "0.12s", "12.3s", "4m12s", "1h05m"
static std::string fmt_dur(double s)
{
    char buf[32];
    if      (s < 1)    std::snprintf(buf, sizeof buf, "%.2fs", s);
    else if (s < 60)   std::snprintf(buf, sizeof buf, "%.1fs", s);
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%dm%02ds", (int)s / 60, (int)s % 60);
    else               std::snprintf(buf, sizeof buf, "%dh%02dm", (int)s / 3600, (int)s % 3600 / 60);
    return buf;
}

static const char* status_name(BucketStats::Status s)
{
    switch (s) {
        case BucketStats::Status::NotRun:    return "not run";
        case BucketStats::Status::Complete:  return "complete";
        case BucketStats::Status::TimeLimit: return "time limit";
        case BucketStats::Status::Budget:    return "node budget";
        case BucketStats::Status::ResultCap: return "result cap";
    }
    return "?";
}

// Coverage report on stderr, relative to a full-vocabulary search: every
// first word of every length, every consistent continuation, no caps.
static void print_report(const CrackStats& st, Report mode)
{
    if (mode == Report::None) return;
    using S = BucketStats::Status;

    // Work is summed over buckets in thread-seconds; threads share buckets, so a
    // full run's wall time is that work spread over the threads.
    double done = 0, left = 0;
    bool lower = false, unknown = false;
    for (const auto& b : st.buckets) {
        done += b.seconds;
        double r = b.est_left();
        if (r < 0) unknown = true;
        else { left += r; lower |= b.est_lower_bound && r > 0; }
    }
    lower |= unknown;

    if (mode != Report::Summary) {
        std::fprintf(stderr, "%3s %5s %5s  %-11s %17s %10s %8s %12s\n", "kl", "first", "stage",
                     "status", "first words", "word prob.", "cpu time", "est. to full");
        for (const auto& b : st.buckets) {
            if (mode == Report::Buckets && b.status == S::Complete) continue;
            char words[32];
            std::snprintf(words, sizeof words, "%d/%d %5.1f%%", b.tried, b.n_words,
                          b.n_words ? 100.0 * b.tried / b.n_words : 100.0);
            double r = b.est_left();
            std::string est = r < 0 ? "?" : (b.est_lower_bound && r > 0 ? ">" : "~") + fmt_dur(r);
            std::fprintf(stderr, "%3d %5d %5d  %-11s %17s %9.1f%% %8s %12s\n",
                         b.kl, b.L, b.stage, status_name(b.status), words, 100.0 * b.mass_tried,
                         b.status == S::NotRun ? "-" : fmt_dur(b.seconds).c_str(), est.c_str());
        }
    }

    const double total = st.calibration + st.search;
    if (left <= 0 && !lower) {
        std::fprintf(stderr, "coverage: full vocabulary searched in %s (%d threads)\n",
                     fmt_dur(total).c_str(), st.threads);
        return;
    }
    // A full run starts from scratch: it redoes this run's work plus the rest.
    const double full = (done + left) / st.threads + st.calibration;
    // Suggested limit: 1.5× the estimate, rounded up to whole minutes.
    const long limit = 60 * (long)std::ceil(1.5 * full / 60);
    std::fprintf(stderr, "coverage: this run searched %s%.0f%% of the full vocabulary (estimated by sampling)\n",
                 lower ? "<" : "~", 100.0 * done / (done + left));
    std::fprintf(stderr, "for full coverage: --time-limit %ld --wcs-budget 0 --wcs-results 0"
                         "  (est. %s%s with %d threads%s)\n",
                 limit, lower ? "at least " : "~", fmt_dur(full).c_str(), st.threads,
                 lower ? "; raise the limit if it is reached" : "");
}

static void cmd_decrypt(const char* path, const Tables& tables,
                        const WcsParams& wcs, Report report)
{
    std::string cipher = to_lower_alpha(read_file(path));
    if (cipher.empty()) { std::cerr << "empty ciphertext\n"; std::exit(1); }
    if ((int)cipher.size() < 4) {
        std::cerr << "ciphertext too short to crack (need >= 4 chars)\n"; std::exit(1);
    }
    if ((int)cipher.size() > MAX_CIPHER_LEN) {
        std::cerr << "ciphertext too long (" << cipher.size() << " letters; max "
                  << MAX_CIPHER_LEN << ")\n";
        std::exit(1);
    }

    CrackStats stats;
    auto candidates = crack(cipher, tables, wcs, &stats);
    print_report(stats, report);

    if (candidates.empty()) {
        std::cout << "no candidates found\n";
        return;
    }

    for (int i = 0; i < (int)candidates.size(); i++) {
        const auto& c = candidates[i];
        std::cout << "--- candidate " << (i+1) << " ---\n";
        std::cout << "key:      " << c.key << '\n';
        std::cout << "plain:    " << c.plain << '\n';
        std::cout << "words:    " << c.segment.formatted << '\n';
        std::cout << "coverage: "
                  << std::fixed << std::setprecision(1)
                  << c.segment.coverage * 100.0 << "%"
                  << "  key_len: " << c.key.size()
                  << "  words: " << c.segment.word_count
                  << "  word_rank: " << std::setprecision(0) << c.segment.word_score
                  << "  fitness: " << std::setprecision(2) << c.fitness
                  << "  lm: " << c.segment.lm
                  << "  score: " << c.score << '\n';
    }
}

static void usage(const char* prog)
{
    std::cerr <<
        "usage: " << prog << " -[ed] <file> [options]\n"
        "\n"
        "mode:\n"
        "  -e   encrypt (reads key from stdin)\n"
        "  -d   decrypt (ciphertext of 4.." << MAX_CIPHER_LEN << " letters, key length 1-12)\n"
        "\n"
        "search tuning (decrypt only):\n"
        "  --time-limit S     stop after S seconds and print what was found (default: 300; 0 = none)\n"
        "  --top N            print the N most probable candidates (default: 10000; 0 = all)\n"
        "  --threads N        worker threads (default: 0 = all hardware threads)\n"
        "  --progress S       print search progress on stderr every S seconds (default: off)\n"
        "  --report MODE      coverage report on stderr: summary, buckets (unfinished\n"
        "                     buckets too), all (every bucket), or none (default: summary)\n"
        "  --wcs-min-word N   shortest first word to try, never above kl-1 (default: 1)\n"
        "  --wcs-results N    max results per key length and first-word length (default: 500; 0 = unlimited)\n"
        "  --wcs-budget N     max word placements per key length and first-word length (default: 500000; 0 = unlimited)\n";
    std::exit(1);
}

int main(int argc, char** argv)
{
    if (argc < 3) usage(argv[0]);

    std::string mode = argv[1];
    if (mode != "-e" && mode != "-d") {
        std::cerr << "unknown mode " << mode << "\n"; usage(argv[0]);
    }

    const char* file = argv[2];
    WcsParams wcs;
    Report    report = Report::Summary;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--wcs-min-word" && i+1 < argc) wcs.min_word = std::stoi(argv[++i]);
        else if (arg == "--wcs-results"  && i+1 < argc) wcs.results  = std::stoi(argv[++i]);
        else if (arg == "--wcs-budget"   && i+1 < argc) wcs.budget   = std::stoi(argv[++i]);
        else if (arg == "--time-limit"   && i+1 < argc) wcs.time_limit = std::stod(argv[++i]);
        else if (arg == "--top"          && i+1 < argc) wcs.top        = std::stoi(argv[++i]);
        else if (arg == "--threads"      && i+1 < argc) wcs.threads    = std::stoi(argv[++i]);
        else if (arg == "--progress"     && i+1 < argc) wcs.progress   = std::stod(argv[++i]);
        else if (arg == "--report"       && i+1 < argc) {
            std::string m = argv[++i];
            if      (m == "summary") report = Report::Summary;
            else if (m == "buckets") report = Report::Buckets;
            else if (m == "all")     report = Report::All;
            else if (m == "none")    report = Report::None;
            else { std::cerr << "unknown report mode: " << m << "\n"; usage(argv[0]); }
        }
        else { std::cerr << "unknown flag: " << arg << "\n"; usage(argv[0]); }
    }

    Tables tables;
    try { tables = load_tables(QUAD_FILE_S.c_str(), DICT_FILE_S.c_str()); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }

    if (mode == "-e") cmd_encrypt(file);
    else              cmd_decrypt(file, tables, wcs, report);
    return 0;
}
