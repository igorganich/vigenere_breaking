#include "rescore.hpp"
#include "tables.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

std::string lm_sentence(const std::string& plain, const std::string& words)
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
    std::string s;
    for (const auto& w : ws) {
        if (!s.empty()) s += ' ';
        s += w == "i" ? "I" : w;
    }
    s[0] = (char)std::toupper((unsigned char)s[0]);
    return s + ".";
}

std::vector<Rescored> rescore(IndexReader& idx, const Gpt2& lm, size_t top, int threads)
{
    const auto cands = idx.range(1, top);
    std::vector<std::string> sentences;
    for (const auto& c : cands) sentences.push_back(lm_sentence(c.plain, c.words));
    const auto lp = lm.logprob(sentences, threads);
    std::vector<Rescored> out;
    for (size_t i = 0; i < cands.size(); i++)
        out.push_back({cands[i].rank, cands[i].key, sentences[i],
                       lp[i] - (double)cands[i].key.size() * std::log((double)ALPHA)});
    std::stable_sort(out.begin(), out.end(), [](const Rescored& a, const Rescored& b){
        return a.score != b.score ? a.score > b.score : a.trigram_rank < b.trigram_rank;
    });
    return out;
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
