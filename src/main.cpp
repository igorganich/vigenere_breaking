#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include "cipher.hpp"
#include "index.hpp"
#include "tables.hpp"

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

static const std::string DICT_FILE = exe_dir() + "/../txt/english_words.txt";

static constexpr int MAX_CIPHER_LEN = 25;

static std::string read_file(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << '\n'; std::exit(1); }
    return {std::istreambuf_iterator<char>(f), {}};
}

static std::string to_lower_alpha(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
        if (std::isalpha(c)) out += (char)std::tolower(c);
    return out;
}

static void usage(const char* prog)
{
    std::cerr <<
        "usage:\n"
        "  " << prog << " -e <plaintext_file>\n"
        "      encrypt; reads the key from stdin\n"
        "  " << prog << " crack <ciphertext_file> <index_file> [options]\n"
        "      find every key (length 1-12) whose plaintext splits fully into dictionary words,\n"
        "      rank them by probability and store them in <index_file>\n"
        "        --top N             print the best N afterwards (default 10)\n"
        "        --max-key-length N  try key lengths 1..N (default 12)\n"
        "        --max-candidates N  skip key lengths that would exceed N candidates in total\n"
        "                            (default 150000000, about 4 GB of memory)\n"
        "        --threads N         worker threads (default: all)\n"
        "  " << prog << " show <index_file> (--count | --first N | --nth N | --range A B | --rank-of PLAINTEXT)\n"
        "      read candidates from an index by rank (rank 1 = most probable)\n";
    std::exit(1);
}

static void print(const Candidate& c)
{
    std::printf("%10zu  key %-12s  %s  %s  score %.2f\n", c.rank, c.key.c_str(), c.plain.c_str(),
                c.words.c_str(), c.score);
}

static Dictionary load()
{
    try { return load_dictionary(DICT_FILE); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; std::exit(1); }
}

static int cmd_encrypt(const char* path)
{
    std::string plain = to_lower_alpha(read_file(path));
    if (plain.empty()) { std::cerr << "empty plaintext\n"; return 1; }
    std::cerr << "key: ";
    std::string key;
    std::cin >> key;
    key = to_lower_alpha(key);
    if (key.empty()) { std::cerr << "invalid key\n"; return 1; }
    for (size_t i = 0; i < plain.size(); i++) std::cout << vig_enc(plain[i], key[i % key.size()]);
    std::cout << '\n';
    return 0;
}

static int cmd_crack(int argc, char** argv)
{
    if (argc < 4) usage(argv[0]);
    IndexOptions opt;
    size_t top = 10;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--threads"        && i + 1 < argc) opt.threads        = std::stoi(argv[++i]);
        else if (a == "--max-key-length" && i + 1 < argc) opt.max_key_length = std::stoi(argv[++i]);
        else if (a == "--top"            && i + 1 < argc) top                = std::stoul(argv[++i]);
        else if (a == "--max-candidates" && i + 1 < argc) opt.max_candidates = std::stoull(argv[++i]);
        else { std::cerr << "unknown option: " << a << '\n'; usage(argv[0]); }
    }
    if (opt.max_key_length < 1 || opt.max_key_length > 12) {
        std::cerr << "--max-key-length must be 1..12\n"; return 1;
    }
    std::string cipher = to_lower_alpha(read_file(argv[2]));
    if (cipher.size() < 2 || cipher.size() > (size_t)MAX_CIPHER_LEN) {
        std::cerr << "ciphertext must have 2.." << MAX_CIPHER_LEN << " letters (has " << cipher.size() << ")\n";
        return 1;
    }
    Dictionary dict = load();
    try {
        build_index(cipher, dict, opt, argv[3]);
        IndexReader idx(argv[3], dict);
        for (const auto& note : idx.summary().notes) std::fprintf(stderr, "note: %s\n", note.c_str());
        for (const auto& c : idx.range(1, top)) print(c);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}

static int cmd_show(int argc, char** argv)
{
    if (argc < 4) usage(argv[0]);
    Dictionary dict = load();
    try {
        IndexReader idx(argv[2], dict);
        const std::string mode = argv[3];
        const size_t count = idx.summary().count;
        if (mode == "--count") {
            std::printf("ciphertext %s: %zu candidates\n", idx.cipher().c_str(), count);
            for (auto [kl, c] : idx.summary().per_key_length) std::printf("  key length %2d: %zu\n", kl, c);
            for (const auto& note : idx.summary().notes) std::printf("  note: %s\n", note.c_str());
            return 0;
        }
        if (mode == "--rank-of" && argc > 4) {
            std::string plain = to_lower_alpha(argv[4]);
            if (size_t r = idx.rank_of(plain)) std::printf("%s: rank %zu of %zu\n", plain.c_str(), r, count);
            else                               std::printf("%s: not among the %zu candidates\n", plain.c_str(), count);
            return 0;
        }
        size_t first, last;
        if      (mode == "--first" && argc > 4) { first = 1; last = std::stoul(argv[4]); }
        else if (mode == "--nth"   && argc > 4) { first = last = std::stoul(argv[4]); }
        else if (mode == "--range" && argc > 5) { first = std::stoul(argv[4]); last = std::stoul(argv[5]); }
        else usage(argv[0]);
        if (first < 1 || first > count) {
            std::cerr << "rank out of range (1.." << count << ")\n"; return 1;
        }
        for (const auto& c : idx.range(first, last)) print(c);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) usage(argv[0]);
    const std::string cmd = argv[1];
    if (cmd == "-e")    return cmd_encrypt(argv[2]);
    if (cmd == "crack") return cmd_crack(argc, argv);
    if (cmd == "show")  return cmd_show(argc, argv);
    usage(argv[0]);
}
