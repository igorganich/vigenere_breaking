#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <iomanip>
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

static void cmd_decrypt(const char* path, const Tables& tables,
                        KeyPriority priority, const WcsParams& wcs)
{
    std::string cipher = to_lower_alpha(read_file(path));
    if (cipher.empty()) { std::cerr << "empty ciphertext\n"; std::exit(1); }
    if ((int)cipher.size() < 4) {
        std::cerr << "ciphertext too short to crack (need >= 4 chars)\n"; std::exit(1);
    }

    auto candidates = crack(cipher, tables, priority, wcs);

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
                  << "  fitness: " << std::setprecision(2) << c.fitness << '\n';
    }
}

static void usage(const char* prog)
{
    std::cerr <<
        "usage: " << prog << " -[ed] <file> [options]\n"
        "\n"
        "mode:\n"
        "  -e   encrypt (reads key from stdin)\n"
        "  -d   decrypt\n"
        "\n"
        "search strategy (decrypt only):\n"
        "  --long-key      run long-key search; skip short-key if high coverage found\n"
        "  --short-key     run short-key stochastic search only\n"
        "  --exhaustive    try all key lengths 1..12, no early exits\n"
        "\n"
        "word_cover_search tuning (only active with --exhaustive):\n"
        "  --wcs-min-word N   minimum first-word length at root (default: kl-1; 0 = no limit)\n"
        "  --wcs-results N    max results per key length      (default: 500;   0 = unlimited)\n"
        "  --wcs-budget N     max recursion nodes per kl      (default: 500000; 0 = unlimited)\n";
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
    KeyPriority priority = KeyPriority::BOTH;
    WcsParams   wcs;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--long-key")   priority = KeyPriority::LONG_FIRST;
        else if (arg == "--short-key")  priority = KeyPriority::SHORT_ONLY;
        else if (arg == "--exhaustive") priority = KeyPriority::EXHAUSTIVE;
        else if (arg == "--wcs-min-word" && i+1 < argc) wcs.min_word = std::stoi(argv[++i]);
        else if (arg == "--wcs-results"  && i+1 < argc) wcs.results  = std::stoi(argv[++i]);
        else if (arg == "--wcs-budget"   && i+1 < argc) wcs.budget   = std::stoi(argv[++i]);
        else { std::cerr << "unknown flag: " << arg << "\n"; usage(argv[0]); }
    }

    Tables tables;
    try { tables = load_tables(QUAD_FILE_S.c_str(), DICT_FILE_S.c_str()); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }

    if (mode == "-e") cmd_encrypt(file);
    else              cmd_decrypt(file, tables, priority, wcs);
    return 0;
}
