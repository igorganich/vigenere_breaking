#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include "cipher.hpp"
#include "gpt2.hpp"
#include "index.hpp"
#include "lang.hpp"
#include "rescore.hpp"
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

static const std::string TXT_DIR    = exe_dir() + "/../txt";       // txt/<lang>/: word list, tables
static const std::string MODELS_DIR = exe_dir() + "/../models";    // models/<gpt2_dir>/

static const Language& language(const std::string& code)
{
    const Language* l = find_language(code);
    if (!l) { std::cerr << "unknown language " << code << " (languages: " << language_codes() << ")\n"; std::exit(1); }
    return *l;
}

static std::string gpt2_dir(const Dictionary& dict) { return MODELS_DIR + "/" + language(dict.lang).gpt2_dir; }

// The language of an existing index; exits with a message if it cannot be read.
static std::string lang_of(const std::string& index_path)
{
    try { return index_lang(index_path); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; std::exit(1); }
}

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
        "      rank them by probability and store them in <index_file>; then rescore the best\n"
        "      with GPT-2 (if models/gpt2 exists: tools/get_gpt2.sh) into <index_file>.lm\n"
        "        --top N             print the best N afterwards (default 10)\n"
        "        --lm-top K          rescore the best K with GPT-2 (default 20000)\n"
        "        --no-lm             skip GPT-2 and print the trigram ranking\n"
        "        --max-key-length N  try key lengths 1..N (default 12)\n"
        "        --max-candidates N  skip key lengths that would exceed N candidates in total\n"
        "                            (default 1000000000; candidates are kept on disk: about\n"
        "                            24 bytes each while building, 8 in the index)\n"
        "        --model M           ranking model: unigram, bigram or trigram (default trigram)\n"
        "        --lang L            plaintext language: en (default) or de (German, with\n"
        "                            ä ö ü ß written ae oe ue ss); later commands read it\n"
        "                            from the index\n"
        "        --words FILE        add the words of FILE (one or more per line, # comments) to the\n"
        "                            dictionary; saved as <index_file>.words for later commands\n"
        "        --threads N         worker threads (default: all)\n"
        "  " << prog << " rerank <index_file> <new_index_file> [--model M] [--threads N] [--top N]\n"
        "      rank the candidates of an index by another model (default trigram), without\n"
        "      repeating the search; the two files may be the same\n"
        "  " << prog << " rescore <index_file> [--top K] [--print N] [--threads N]\n"
        "      rescore the best K (default 20000) of an index with GPT-2 into <index_file>.lm\n"
        "      and print the best N (default 10)\n"
        "        --ranks FILE --scores OUT  instead rescore the candidates at the trigram ranks\n"
        "                            listed in FILE (one per line), appending each batch of\n"
        "                            20,000 to OUT; rerun to resume after an interruption\n"
        "  " << prog << " show <index_file> (--count | --first N | --nth N | --range A B | --rank-of PLAINTEXT\n"
        "                                  | --lm N)\n"
        "      read candidates from an index by rank (rank 1 = most probable), scored by the\n"
        "      model the index is ranked by; --lm N: the best N of the GPT-2 shortlist\n";
    std::exit(1);
}

static void print(const Candidate& c)
{
    std::printf("%10zu  key %-12s  %s  %s  score %.2f\n", c.rank, c.key.c_str(), c.plain.c_str(),
                c.words.c_str(), c.score);
}

static void print_lm(size_t rank, const Rescored& r)
{
    std::printf("%10zu  key %-12s  %-44s  trigram #%-7zu  score %.2f\n", rank, r.key.c_str(), r.sentence.c_str(),
                r.trigram_rank, r.score);
}

// Rescore the best `top` of an index with GPT-2, save the shortlist, print the best `print_n`.
static void run_rescore(IndexReader& idx, const std::string& path, size_t top, size_t print_n, int threads)
{
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    Gpt2 lm(gpt2_dir(idx.dict()));
    auto t1 = Clock::now();
    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
    const auto list = rescore(idx, lm, top, threads);
    write_shortlist(path, idx, list);
    std::fprintf(stderr, "rescored the best %zu with GPT-2 (%.1fs, loading %.1fs): %s\n", list.size(),
                 std::chrono::duration<double>(Clock::now() - t1).count(),
                 std::chrono::duration<double>(t1 - t0).count(), shortlist_path(path).c_str());
    for (size_t i = 0; i < std::min(print_n, list.size()); i++) print_lm(i + 1, list[i]);
}

// Custom words of an index are kept next to it, so every command reads it with the same
// dictionary it was built with.
static std::string words_path(const std::string& index_path) { return index_path + ".words"; }

// The dictionary of a language plus custom words: from `words_file` if given, else those
// saved next to `index_path` (if any).
static Dictionary load(const std::string& lang, const std::string& words_file = "", const std::string& index_path = "")
{
    const Language& l = language(lang);
    try {
        std::vector<std::string> extra;
        if (!words_file.empty()) extra = read_word_file(words_file);
        else if (!index_path.empty() && std::ifstream(words_path(index_path))) extra = read_word_file(words_path(index_path));
        Dictionary d = load_dictionary(TXT_DIR + "/" + l.code + "/words.txt", extra, l.singles);
        d.lang = l.code;
        load_display(d, TXT_DIR + "/" + l.code + "/display.txt");
        return d;
    }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; std::exit(1); }
}

// Save the custom words of `dict` next to `index_path`, or remove a stale list.
static void save_words(const Dictionary& dict, const std::string& index_path)
{
    const std::string p = words_path(index_path);
    std::remove(p.c_str());
    if (dict.extra.empty()) return;
    std::ofstream f(p);
    for (const auto& w : dict.extra) f << w << '\n';
    if (!f) throw std::runtime_error("cannot write " + p);
}

static LanguageModel load_lm(const Dictionary& dict, ModelKind kind)
{
    try { return load_model(kind, dict, TXT_DIR + "/" + dict.lang); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; std::exit(1); }
}

static ModelKind model_arg(const char* s)
{
    ModelKind k;
    if (!parse_model_kind(s, &k)) { std::cerr << "unknown model " << s << " (models: " << MODEL_NAMES << ")\n"; std::exit(1); }
    return k;
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
    size_t top = 10, lm_top = 20000;
    bool use_lm = true;
    ModelKind kind = ModelKind::Trigram;
    std::string words_file, lang = "en";
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--threads"        && i + 1 < argc) opt.threads        = std::stoi(argv[++i]);
        else if (a == "--lang"           && i + 1 < argc) lang               = argv[++i];
        else if (a == "--words"          && i + 1 < argc) words_file         = argv[++i];
        else if (a == "--lm-top"         && i + 1 < argc) lm_top             = std::stoul(argv[++i]);
        else if (a == "--no-lm")                          use_lm             = false;
        else if (a == "--model"          && i + 1 < argc) kind               = model_arg(argv[++i]);
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
    Dictionary dict = load(lang, words_file);
    if (!dict.extra.empty()) std::fprintf(stderr, "dictionary: %zu custom words from %s\n", dict.extra.size(), words_file.c_str());
    LanguageModel model = load_lm(dict, kind);
    try {
        std::remove(shortlist_path(argv[3]).c_str());       // a shortlist of the old index
        save_words(dict, argv[3]);
        try { build_index(cipher, dict, model, opt, argv[3]); }
        catch (const std::bad_alloc&) {
            throw std::runtime_error("out of memory in the search; try a smaller --max-key-length");
        }
        IndexReader idx(argv[3], dict, model);
        for (const auto& note : idx.summary().notes) std::fprintf(stderr, "note: %s\n", note.c_str());
        if (use_lm && lm_top > 0 && Gpt2::available(gpt2_dir(dict))) {
            run_rescore(idx, argv[3], lm_top, top, opt.threads);
        } else {
            if (use_lm && lm_top > 0)
                std::fprintf(stderr, "note: GPT-2 not found in %s (run %s); "
                             "showing the %s ranking\n", gpt2_dir(dict).c_str(), language(lang).get_gpt2.c_str(),
                             model.name.c_str());
            for (const auto& c : idx.range(1, top)) print(c);
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}

static int cmd_rerank(int argc, char** argv)
{
    if (argc < 4) usage(argv[0]);
    ModelKind kind = ModelKind::Trigram;
    int threads = 0;
    size_t top = 10;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--model"   && i + 1 < argc) kind    = model_arg(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::stoi(argv[++i]);
        else if (a == "--top"     && i + 1 < argc) top     = std::stoul(argv[++i]);
        else { std::cerr << "unknown option: " << a << '\n'; usage(argv[0]); }
    }
    Dictionary dict = load(lang_of(argv[2]), "", argv[2]);
    LanguageModel model = load_lm(dict, kind);
    try {
        std::remove(shortlist_path(argv[3]).c_str());       // ranked by the old order
        if (std::string(argv[2]) != argv[3]) save_words(dict, argv[3]);
        rerank_index(argv[2], argv[3], dict, model, threads);
        IndexReader idx(argv[3], dict, model);
        for (const auto& c : idx.range(1, top)) print(c);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}

// The model an existing index is ranked by.
static LanguageModel index_model(const Dictionary& dict, const std::string& path)
{
    ModelKind kind;
    const std::string name = index_model_name(path);
    if (name == "zipf")
        throw std::runtime_error(path + " is ranked by the removed zipf model; update it with `rerank`");
    if (!parse_model_kind(name, &kind)) throw std::runtime_error(path + ": unknown model " + name);
    return load_lm(dict, kind);
}

static int cmd_rescore(int argc, char** argv)
{
    if (argc < 3) usage(argv[0]);
    size_t top = 20000, print_n = 10;
    int threads = 0;
    std::string ranks_file, scores_file;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--top"     && i + 1 < argc) top     = std::stoul(argv[++i]);
        else if (a == "--ranks"   && i + 1 < argc) ranks_file  = argv[++i];
        else if (a == "--scores"  && i + 1 < argc) scores_file = argv[++i];
        else if (a == "--print"   && i + 1 < argc) print_n = std::stoul(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::stoi(argv[++i]);
        else { std::cerr << "unknown option: " << a << '\n'; usage(argv[0]); }
    }
    Dictionary dict = load(lang_of(argv[2]), "", argv[2]);
    if (!Gpt2::available(gpt2_dir(dict))) {
        std::cerr << "GPT-2 not found in " << gpt2_dir(dict) << " (run " << language(dict.lang).get_gpt2 << ")\n";
        return 1;
    }
    try {
        LanguageModel model = index_model(dict, argv[2]);
        IndexReader idx(argv[2], dict, model);
        if (ranks_file.empty() != scores_file.empty()) throw std::runtime_error("--ranks and --scores go together");
        if (!ranks_file.empty()) {
            std::ifstream rf(ranks_file);
            if (!rf) throw std::runtime_error("cannot open " + ranks_file);
            std::vector<size_t> ranks;
            for (size_t r; rf >> r; ) {
                if (r < 1 || r > idx.summary().count) throw std::runtime_error(ranks_file + ": rank out of range");
                ranks.push_back(r);
            }
            if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
            Gpt2 lm(gpt2_dir(dict));
            const auto start = std::chrono::steady_clock::now();
            size_t first_done = SIZE_MAX;
            const auto list = rescore_ranks(idx, lm, ranks, scores_file, threads, 20000, [&](size_t done, size_t total){
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (first_done == SIZE_MAX) first_done = done - std::min<size_t>(done, 20000);
                const double rate = (done - first_done) / std::max(secs, 1e-9);
                std::fprintf(stderr, "rescored %zu of %zu (%.0f/s, %.1f h left)\n", done, total, rate,
                             (total - done) / std::max(rate, 1e-9) / 3600);
            });
            std::fprintf(stderr, "%zu scores in %s\n", list.size(), scores_file.c_str());
            for (size_t i = 0; i < std::min(print_n, list.size()); i++) print_lm(i + 1, list[i]);
            return 0;
        }
        run_rescore(idx, argv[2], top, print_n, threads);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}

static int cmd_show(int argc, char** argv)
{
    if (argc < 4) usage(argv[0]);
    Dictionary dict = load(lang_of(argv[2]), "", argv[2]);
    try {
        LanguageModel model = index_model(dict, argv[2]);
        IndexReader idx(argv[2], dict, model);
        const std::string mode = argv[3];
        const size_t count = idx.summary().count;
        if (mode == "--lm") {
            const size_t n = argc > 4 ? std::stoul(argv[4]) : 10;
            const auto list = read_shortlist(argv[2], idx);
            for (size_t i = 0; i < std::min(n, list.size()); i++) print_lm(i + 1, list[i]);
            return 0;
        }
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
    if (cmd == "rerank") return cmd_rerank(argc, argv);
    if (cmd == "rescore") return cmd_rescore(argc, argv);
    if (cmd == "show")  return cmd_show(argc, argv);
    usage(argv[0]);
}
