#include "rescore.hpp"
#include "tables.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

// `s` with its first letter in capitals: ASCII, Latin-1 (ä, ö, ü -> Ä, Ö, Ü) and Cyrillic
// (а-я, є, і, ї, ґ -> А-Я, Є, І, Ї, Ґ), all in UTF-8.
std::string capitalize_first(std::string s)
{
    if (s.empty()) return s;
    unsigned char& a = (unsigned char&)s[0];
    if (a < 0x80) { a = (unsigned char)std::toupper(a); return s; }
    if (s.size() < 2) return s;
    unsigned char& b = (unsigned char&)s[1];
    if (a == 0xC3 && b >= 0xA0 && b <= 0xBE && b != 0xB7) b -= 0x20;     // à-þ -> À-Þ, not ÷
    else if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) b -= 0x20;             // а-п -> А-П
    else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { a = 0xD0; b += 0x20; }  // р-я -> Р-Я
    else if (a == 0xD1 && b >= 0x90 && b <= 0x9F) { a = 0xD0; b -= 0x10; }  // ѐ-џ (є, і, ї) -> Ѐ-Џ
    else if (a == 0xD2 && b == 0x91) b = 0x90;                           // ґ -> Ґ
    return s;
}

} // namespace

std::string lm_sentence(const std::string& plain, const std::string& words, const Dictionary* dict)
{
    // Words from "[w:r][w:r]..."; a split with a stray letter falls back to the plaintext.
    std::vector<std::string> ws;
    bool stray = false;
    for (size_t i = 0; i < words.size(); ) {
        if (words[i] != '[') { stray = true; break; }
        const size_t colon = words.find(':', i), close = words.find(']', i);
        if (colon == std::string::npos || close == std::string::npos) { stray = true; break; }
        ws.push_back(words.substr(i + 1, colon - i - 1));
        i = close + 1;
    }
    if (stray || ws.empty()) ws = {plain};
    const bool english = !dict || dict->lang == "en";
    std::string s;
    for (const auto& w : ws) {
        if (!s.empty()) s += ' ';
        if (dict && dict->display.count(w)) s += dict->display.at(w);
        else if (english && w == "i") s += "I";
        else s += dict ? dict->alphabet.decode(w) : w;
    }
    return capitalize_first(s) + ".";
}

std::vector<Rescored> rescore(IndexReader& idx, const Gpt2& lm, size_t top, int threads)
{
    const auto cands = idx.range(1, top);
    std::vector<std::string> sentences;
    for (const auto& c : cands) sentences.push_back(lm_sentence(c.plain, c.words, &idx.dict()));
    const auto lp = lm.logprob(sentences, threads);
    std::vector<Rescored> out;
    for (size_t i = 0; i < cands.size(); i++)
        out.push_back({cands[i].rank, idx.dict().alphabet.decode(cands[i].key), sentences[i],
                       lp[i] - (double)cands[i].key.size() * std::log((double)idx.dict().alphabet.size())});
    std::stable_sort(out.begin(), out.end(), [](const Rescored& a, const Rescored& b){
        return a.score != b.score ? a.score > b.score : a.trigram_rank < b.trigram_rank;
    });
    return out;
}

namespace {

void sort_rescored(std::vector<Rescored>& v)
{
    std::stable_sort(v.begin(), v.end(), [](const Rescored& a, const Rescored& b){
        return a.score != b.score ? a.score > b.score : a.trigram_rank < b.trigram_rank;
    });
}

std::vector<Rescored> read_scores(const std::string& path)
{
    std::vector<Rescored> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        Rescored r;
        std::string score;
        if (!(in >> r.trigram_rank >> r.key >> score)) continue;   // a line cut off by an interruption
        r.score = std::stod(score);
        std::getline(in >> std::ws, r.sentence);
        if (!r.sentence.empty() && r.sentence.back() == '.') out.push_back(r);
    }
    return out;
}

} // namespace

std::vector<Rescored> rescore_ranks(IndexReader& idx, const Gpt2& lm, const std::vector<size_t>& ranks,
                                    const std::string& scores_path, int threads, size_t batch,
                                    const std::function<void(size_t, size_t)>& progress)
{
    std::vector<size_t> done_ranks;
    for (const auto& r : read_scores(scores_path)) done_ranks.push_back(r.trigram_rank);
    std::sort(done_ranks.begin(), done_ranks.end());
    std::vector<size_t> todo;
    for (size_t r : ranks)
        if (!std::binary_search(done_ranks.begin(), done_ranks.end(), r)) todo.push_back(r);
    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());

    std::ofstream f(scores_path, std::ios::app);
    if (!f) throw std::runtime_error("cannot write " + scores_path);
    const size_t total = done_ranks.size() + todo.size();
    for (size_t i = 0; i < todo.size(); i += batch) {
        std::vector<Candidate> cands;
        std::vector<std::string> sentences;
        for (size_t j = i; j < std::min(todo.size(), i + batch); j++) {
            cands.push_back(idx.range(todo[j], todo[j]).at(0));
            sentences.push_back(lm_sentence(cands.back().plain, cands.back().words, &idx.dict()));
        }
        const auto lp = lm.logprob(sentences, threads);
        char buf[64];
        for (size_t j = 0; j < cands.size(); j++) {
            std::snprintf(buf, sizeof buf, "%.17g", lp[j] - (double)cands[j].key.size() * std::log((double)idx.dict().alphabet.size()));
            f << cands[j].rank << '\t' << idx.dict().alphabet.decode(cands[j].key) << '\t' << buf << '\t' << sentences[j] << '\n';
        }
        if (!f.flush()) throw std::runtime_error("error writing " + scores_path);
        progress(done_ranks.size() + std::min(todo.size(), i + batch), total);
    }
    f.close();
    auto all = read_scores(scores_path);
    sort_rescored(all);
    return all;
}

std::string shortlist_path(const std::string& index_path) { return index_path + ".lm"; }

namespace {

std::string identity(const IndexReader& idx)
{
    return "cipher " + idx.cipher() + "\ncandidates " + std::to_string(idx.summary().count) +
           "\nranking " + idx.model().name + " " + idx.model().fingerprint + "\n";
}

} // namespace

void write_shortlist(const std::string& index_path, const IndexReader& idx, const std::vector<Rescored>& list)
{
    const std::string path = shortlist_path(index_path), tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) throw std::runtime_error("cannot write " + tmp);
        f << "VIGLM 1\n" << identity(idx) << "rescored " << list.size() << " gpt2\nend\n";
        char buf[64];
        for (const auto& r : list) {
            std::snprintf(buf, sizeof buf, "%.17g", r.score);
            f << r.trigram_rank << '\t' << r.key << '\t' << buf << '\t' << r.sentence << '\n';
        }
        if (!f.flush()) throw std::runtime_error("error writing " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("cannot write " + path);
    }
}

std::vector<Rescored> read_shortlist(const std::string& index_path, const IndexReader& idx)
{
    const std::string path = shortlist_path(index_path);
    std::ifstream f(path);
    if (!f) throw std::runtime_error("no GPT-2 shortlist " + path + "; make one with `rescore`");
    std::string line, head;
    while (std::getline(f, line) && line != "end") head += line + "\n";
    if (head.rfind("VIGLM 1\n", 0) != 0) throw std::runtime_error(path + " is not a GPT-2 shortlist");
    if (head.find(identity(idx)) == std::string::npos)
        throw std::runtime_error(path + " belongs to an earlier version of the index; make it again with `rescore`");
    std::vector<Rescored> out;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        Rescored r;
        std::string score;
        if (!(in >> r.trigram_rank >> r.key >> score)) throw std::runtime_error(path + ": malformed line");
        r.score = std::stod(score);
        std::getline(in >> std::ws, r.sentence);
        out.push_back(r);
    }
    return out;
}
