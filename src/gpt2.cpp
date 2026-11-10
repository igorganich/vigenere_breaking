#include "gpt2.hpp"
#include "dp.hpp"          // parallel_for
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#define GPT2_AVX2 1
#endif

namespace {

constexpr int NR = 24;      // columns per weight panel (three 8-float AVX registers)
constexpr int MR = 4;       // rows per micro-kernel step
constexpr int ROW_BLOCK = 64;

// ── Minimal JSON (objects, arrays, strings, numbers, literals) ────────────────

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& key) const {
        for (const auto& [k, v] : obj) if (k == key) return &v;
        return nullptr;
    }
};

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F));
        out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

class JsonParser {
public:
    JsonParser(const std::string& s, const std::string& what) : s_(s), what_(what) {}
    Json parse() { Json v = value(); ws(); if (i_ != s_.size()) fail(); return v; }

private:
    [[noreturn]] void fail() { throw std::runtime_error(what_ + ": malformed JSON at byte " + std::to_string(i_)); }
    void ws() { while (i_ < s_.size() && std::strchr(" \t\r\n", s_[i_])) i_++; }
    char peek() { ws(); if (i_ >= s_.size()) fail(); return s_[i_]; }
    void expect(char c) { if (peek() != c) fail(); i_++; }

    uint32_t hex4() {
        if (i_ + 4 > s_.size()) fail();
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_++];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : (fail(), 0));
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail();
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail();
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;   case '\\': out += '\\'; break;
                case '/': out += '/'; break;   case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {          // surrogate pair
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') fail();
                        i_ += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail();
            }
        }
    }
    Json value() {
        Json v;
        char c = peek();
        if (c == '{') {
            v.type = Json::Object;
            i_++;
            if (peek() == '}') { i_++; return v; }
            while (true) {
                std::string k = string();
                expect(':');
                v.obj.emplace_back(std::move(k), value());
                if (peek() == ',') { i_++; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            i_++;
            if (peek() == ']') { i_++; return v; }
            while (true) {
                v.arr.push_back(value());
                if (peek() == ',') { i_++; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') { v.type = Json::String; v.str = string(); return v; }
        for (const char* lit : {"true", "false", "null"})
            if (s_.compare(i_, std::strlen(lit), lit) == 0) {
                i_ += std::strlen(lit);
                v.type = lit[0] == 'n' ? Json::Null : Json::Bool;
                v.num = lit[0] == 't';
                return v;
            }
        size_t start = i_;
        while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) i_++;
        if (start == i_) fail();
        v.type = Json::Number;
        v.num = std::stod(s_.substr(start, i_ - start));
        return v;
    }

    const std::string& s_;
    std::string what_;
    size_t i_ = 0;
};

std::string read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

// ── safetensors ───────────────────────────────────────────────────────────────

class SafeTensors {
public:
    explicit SafeTensors(const std::string& path) : path_(path), f_(path, std::ios::binary) {
        if (!f_) throw std::runtime_error("cannot open " + path);
        uint64_t n = 0;
        f_.read((char*)&n, 8);
        if (!f_ || n > (64u << 20)) throw std::runtime_error(path + ": not a safetensors file");
        std::string h(n, '\0');
        f_.read(&h[0], (std::streamsize)n);
        if (!f_) throw std::runtime_error(path + ": truncated header");
        header_ = JsonParser(h, path).parse();
        base_ = 8 + n;
    }
    // A float32 tensor with the given number of elements.
    std::vector<float> get(const std::string& name, size_t count) {
        const Json* t = header_.get(name);
        if (!t) throw std::runtime_error(path_ + ": no tensor " + name);
        const Json* dtype = t->get("dtype");
        const Json* off = t->get("data_offsets");
        if (!dtype || dtype->str != "F32" || !off || off->arr.size() != 2)
            throw std::runtime_error(path_ + ": tensor " + name + " is not float32");
        const uint64_t a = (uint64_t)off->arr[0].num, b = (uint64_t)off->arr[1].num;
        if (b - a != count * sizeof(float))
            throw std::runtime_error(path_ + ": tensor " + name + " has an unexpected size");
        std::vector<float> v(count);
        f_.seekg((std::streamoff)(base_ + a));
        f_.read((char*)v.data(), (std::streamsize)(b - a));
        if (!f_) throw std::runtime_error(path_ + ": truncated tensor " + name);
        return v;
    }
    bool has(const std::string& name) const { return header_.get(name) != nullptr; }
    std::vector<int> shape(const std::string& name) {
        const Json* t = header_.get(name);
        if (!t || !t->get("shape")) throw std::runtime_error(path_ + ": no tensor " + name);
        std::vector<int> s;
        for (const auto& x : t->get("shape")->arr) s.push_back((int)x.num);
        return s;
    }

private:
    std::string   path_;
    std::ifstream f_;
    Json          header_;
    uint64_t      base_ = 0;
};

// ── Matrix products ───────────────────────────────────────────────────────────

// Pack a row-major K x N matrix (or, with `transposed`, an N x K one read as its transpose).
Gpt2::Packed pack(const std::vector<float>& w, int K, int N, bool transposed, const std::vector<float>& bias)
{
    Gpt2::Packed p;
    p.K = K; p.N = N; p.panels = (N + NR - 1) / NR;
    p.data.assign((size_t)p.panels * K * NR, 0.0f);
    for (int k = 0; k < K; k++)
        for (int n = 0; n < N; n++)
            p.data[((size_t)(n / NR) * K + k) * NR + n % NR] = transposed ? w[(size_t)n * K + k] : w[(size_t)k * N + n];
    if (!bias.empty()) {
        p.bias.assign((size_t)p.panels * NR, 0.0f);
        std::copy(bias.begin(), bias.end(), p.bias.begin());
    }
    return p;
}

// out[MR][NR] = A[i .. i+MR) · panel, accumulated over k in increasing order.
inline void micro_kernel(const float* A, int K, const float* panel, float out[MR][NR])
{
#ifdef GPT2_AVX2
    __m256 c[MR][3];
    for (int r = 0; r < MR; r++) for (int j = 0; j < 3; j++) c[r][j] = _mm256_setzero_ps();
    const float* a[MR];
    for (int r = 0; r < MR; r++) a[r] = A + (size_t)r * K;
    for (int k = 0; k < K; k++) {
        const __m256 b0 = _mm256_loadu_ps(panel), b1 = _mm256_loadu_ps(panel + 8), b2 = _mm256_loadu_ps(panel + 16);
        panel += NR;
        for (int r = 0; r < MR; r++) {
            const __m256 x = _mm256_broadcast_ss(a[r] + k);
            c[r][0] = _mm256_fmadd_ps(x, b0, c[r][0]);
            c[r][1] = _mm256_fmadd_ps(x, b1, c[r][1]);
            c[r][2] = _mm256_fmadd_ps(x, b2, c[r][2]);
        }
    }
    for (int r = 0; r < MR; r++) for (int j = 0; j < 3; j++) _mm256_storeu_ps(out[r] + 8 * j, c[r][j]);
#else
    for (int r = 0; r < MR; r++) for (int j = 0; j < NR; j++) out[r][j] = 0;
    for (int k = 0; k < K; k++, panel += NR)
        for (int r = 0; r < MR; r++) {
            const float x = A[(size_t)r * K + k];
            for (int j = 0; j < NR; j++) out[r][j] += x * panel[j];
        }
#endif
}

// C[M x N] = A[M x K] · B (+ bias).  M must be a multiple of MR.
void matmul(const float* A, int M, const Gpt2::Packed& B, float* C, int threads)
{
    const int row_blocks = (M + ROW_BLOCK - 1) / ROW_BLOCK;
    parallel_for((size_t)row_blocks * B.panels, threads, [&](size_t task){
        const int p = (int)(task % B.panels), r0 = (int)(task / B.panels) * ROW_BLOCK;
        const int r1 = std::min(M, r0 + ROW_BLOCK), n0 = p * NR, cols = std::min(NR, B.N - n0);
        const float* panel = B.data.data() + (size_t)p * B.K * NR;
        float out[MR][NR];
        for (int i = r0; i < r1; i += MR) {
            micro_kernel(A + (size_t)i * B.K, B.K, panel, out);
            for (int r = 0; r < MR; r++) {
                float* c = C + (size_t)(i + r) * B.N + n0;
                for (int j = 0; j < cols; j++) c[j] = out[r][j] + (B.bias.empty() ? 0.0f : B.bias[n0 + j]);
            }
        }
    });
}

void layer_norm(const float* x, float* y, int M, int D, const std::vector<float>& g, const std::vector<float>& b,
                int threads)
{
    parallel_for((size_t)M, threads, [&](size_t r){
        const float* xr = x + r * D;
        double mean = 0, var = 0;
        for (int d = 0; d < D; d++) mean += xr[d];
        mean /= D;
        for (int d = 0; d < D; d++) var += (xr[d] - mean) * (xr[d] - mean);
        const float inv = (float)(1.0 / std::sqrt(var / D + 1e-5)), m = (float)mean;
        float* yr = y + r * D;
        for (int d = 0; d < D; d++) yr[d] = (xr[d] - m) * inv * g[d] + b[d];
    });
}

} // namespace

// ── Loading ───────────────────────────────────────────────────────────────────

bool Gpt2::available(const std::string& dir)
{
    if (!std::ifstream(dir + "/model.safetensors")) return false;
    return (std::ifstream(dir + "/vocab.json") && std::ifstream(dir + "/merges.txt")) ||
           std::ifstream(dir + "/tokenizer.json");
}

Gpt2::Gpt2(const std::string& dir)
{
    // Tokenizer: the byte -> symbol table of GPT-2's byte-level BPE, the vocabulary, the merges.
    {
        std::vector<int> bs;
        for (int b = '!'; b <= '~'; b++) bs.push_back(b);
        for (int b = 0xA1; b <= 0xAC; b++) bs.push_back(b);
        for (int b = 0xAE; b <= 0xFF; b++) bs.push_back(b);
        std::vector<bool> direct(256, false);
        for (int b : bs) direct[b] = true;
        int extra = 0;
        for (int b = 0; b < 256; b++) {
            byte_symbol_[b].clear();
            append_utf8(byte_symbol_[b], direct[b] ? (uint32_t)b : (uint32_t)(256 + extra++));
        }
        // OpenAI's release has vocab.json and merges.txt; others (dbmdz/german-gpt2) only a
        // Hugging Face tokenizer.json, whose BPE model holds the same vocabulary and merges.
        if (std::ifstream(dir + "/vocab.json")) {
            const std::string vocab_text = read_file(dir + "/vocab.json");
            const Json vocab = JsonParser(vocab_text, dir + "/vocab.json").parse();
            for (const auto& [tok, id] : vocab.obj) vocab_[tok] = (int)id.num;
            std::ifstream m(dir + "/merges.txt");
            if (!m) throw std::runtime_error("cannot open " + dir + "/merges.txt");
            std::string line;
            int rank = 0;
            while (std::getline(m, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line.rfind("#version", 0) == 0) continue;
                merge_rank_.emplace(line, rank++);
            }
        } else {
            const std::string path = dir + "/tokenizer.json";
            const Json tok = JsonParser(read_file(path), path).parse();
            const Json* model = tok.get("model");
            const Json* vocab = model ? model->get("vocab") : nullptr;
            const Json* merges = model ? model->get("merges") : nullptr;
            if (!vocab || !merges) throw std::runtime_error(path + ": no BPE vocabulary and merges");
            for (const auto& [t, id] : vocab->obj) vocab_[t] = (int)id.num;
            int rank = 0;
            for (const auto& m : merges->arr)          // "a b", or ["a", "b"] in newer files
                merge_rank_.emplace(m.type == Json::String ? m.str : m.arr.at(0).str + " " + m.arr.at(1).str, rank++);
        }
        // The context of the first token: GPT-2's end-of-text token, or <s> in models that
        // use it as their beginning-of-sequence token.
        auto eot = vocab_.find("<|endoftext|>");
        if (eot == vocab_.end()) eot = vocab_.find("<s>");
        if (eot == vocab_.end()) throw std::runtime_error(dir + ": no <|endoftext|> or <s> token");
        eot_ = eot->second;
    }

    SafeTensors st(dir + "/model.safetensors");
    const std::string pre = st.has("wte.weight") ? "" : "transformer.";   // GPT2Model or GPT2LMHeadModel names
    auto name = [&](const std::string& n) { return pre + n; };
    const auto wte_shape = st.shape(name("wte.weight")), wpe_shape = st.shape(name("wpe.weight"));
    if (wte_shape.size() != 2 || wpe_shape.size() != 2 || wte_shape[1] != n_embd_ || wpe_shape[1] != n_embd_)
        throw std::runtime_error(dir + "/model.safetensors: not GPT-2 small (n_embd 768)");
    n_vocab_ = wte_shape[0];
    n_ctx_   = wpe_shape[0];
    const int D = n_embd_;
    wte_ = st.get(name("wte.weight"), (size_t)n_vocab_ * D);
    wpe_ = st.get(name("wpe.weight"), (size_t)n_ctx_ * D);
    head_ = pack(wte_, D, n_vocab_, true, {});
    lnf_g_ = st.get(name("ln_f.weight"), D);
    lnf_b_ = st.get(name("ln_f.bias"), D);
    for (int l = 0; l < n_layer_; l++) {
        const std::string p = name("h." + std::to_string(l) + ".");
        Layer L;
        L.ln1_g = st.get(p + "ln_1.weight", D);
        L.ln1_b = st.get(p + "ln_1.bias", D);
        L.ln2_g = st.get(p + "ln_2.weight", D);
        L.ln2_b = st.get(p + "ln_2.bias", D);
        L.attn = pack(st.get(p + "attn.c_attn.weight", (size_t)D * 3 * D), D, 3 * D, false, st.get(p + "attn.c_attn.bias", 3 * D));
        L.proj = pack(st.get(p + "attn.c_proj.weight", (size_t)D * D), D, D, false, st.get(p + "attn.c_proj.bias", D));
        L.fc   = pack(st.get(p + "mlp.c_fc.weight", (size_t)D * 4 * D), D, 4 * D, false, st.get(p + "mlp.c_fc.bias", 4 * D));
        L.out  = pack(st.get(p + "mlp.c_proj.weight", (size_t)4 * D * D), 4 * D, D, false, st.get(p + "mlp.c_proj.bias", D));
        layers_.push_back(std::move(L));
    }
}

// ── Tokenizer ─────────────────────────────────────────────────────────────────

std::vector<int> Gpt2::encode(const std::string& text) const
{
    // Pre-tokenization: GPT-2's pattern  's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+|
    // ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+.  Every byte of a non-ASCII UTF-8 character counts as
    // a letter: the sentences read here are words (ä, ö, ü, ß, Cyrillic) and ASCII punctuation.
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    auto is_alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::vector<std::string> chunks;
    const size_t n = text.size();
    for (size_t i = 0; i < n; ) {
        if (text[i] == '\'') {
            bool done = false;
            for (const char* c : {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"})
                if (text.compare(i, std::strlen(c), c) == 0) { chunks.push_back(c); i += std::strlen(c); done = true; break; }
            if (done) continue;
        }
        const size_t j = (text[i] == ' ' && i + 1 < n && !is_space(text[i + 1])) ? i + 1 : i;
        if (!is_space(text[j])) {
            size_t e = j + 1;
            if (is_alpha(text[j]))      while (e < n && is_alpha(text[e])) e++;
            else if (is_digit(text[j])) while (e < n && is_digit(text[e])) e++;
            else while (e < n && !is_space(text[e]) && !is_alpha(text[e]) && !is_digit(text[e])) e++;
            chunks.push_back(text.substr(i, e - i));
            i = e;
            continue;
        }
        size_t e = i;
        while (e < n && is_space(text[e])) e++;
        if (e < n && e - i > 1) e--;          // \s+(?!\S): leave the last space to the next word
        chunks.push_back(text.substr(i, e - i));
        i = e;
    }

    // Byte-level BPE on each chunk: repeatedly merge the adjacent pair with the best rank.
    std::vector<int> ids;
    for (const auto& chunk : chunks) {
        std::vector<std::string> word;
        for (unsigned char c : chunk) word.push_back(byte_symbol_[c]);
        while (word.size() > 1) {
            int best = INT_MAX;
            size_t at = 0;
            for (size_t k = 0; k + 1 < word.size(); k++) {
                auto it = merge_rank_.find(word[k] + " " + word[k + 1]);
                if (it != merge_rank_.end() && it->second < best) { best = it->second; at = k; }
            }
            if (best == INT_MAX) break;
            const std::string a = word[at], b = word[at + 1];
            std::vector<std::string> merged;
            for (size_t k = 0; k < word.size(); k++) {
                if (k + 1 < word.size() && word[k] == a && word[k + 1] == b) { merged.push_back(a + b); k++; }
                else merged.push_back(word[k]);
            }
            word.swap(merged);
        }
        for (const auto& sym : word) {
            auto it = vocab_.find(sym);
            if (it == vocab_.end()) throw std::runtime_error("GPT-2 tokenizer: no token for \"" + sym + "\"");
            ids.push_back(it->second);
        }
    }
    return ids;
}

// ── Scoring ───────────────────────────────────────────────────────────────────

std::vector<double> Gpt2::logprob(const std::vector<std::string>& texts, int threads) const
{
    std::vector<std::vector<int>> seqs;
    for (const auto& t : texts) {
        std::vector<int> s{eot_};
        auto ids = encode(t);
        s.insert(s.end(), ids.begin(), ids.end());
        if ((int)s.size() > n_ctx_) throw std::runtime_error("GPT-2: text longer than the context");
        seqs.push_back(std::move(s));
    }
    // Batches of about 1024 positions; every value is computed independently of the batch.
    std::vector<double> out;
    for (size_t i = 0; i < seqs.size(); ) {
        std::vector<std::vector<int>> batch;
        size_t rows = 0;
        while (i < seqs.size() && (batch.empty() || rows + seqs[i].size() <= 1024)) {
            rows += seqs[i].size();
            batch.push_back(seqs[i++]);
        }
        auto lp = forward(batch, threads);
        out.insert(out.end(), lp.begin(), lp.end());
    }
    return out;
}

std::vector<double> Gpt2::forward(const std::vector<std::vector<int>>& seqs, int threads) const
{
    const int D = n_embd_, H = n_head_, hd = D / H;
    std::vector<int> start;        // first row of each sequence
    int M = 0;
    for (const auto& s : seqs) { start.push_back(M); M += (int)s.size(); }
    const int Mp = (M + MR - 1) / MR * MR;    // rows padded to the micro-kernel

    std::vector<float> x((size_t)Mp * D, 0.0f), h((size_t)Mp * D, 0.0f), a((size_t)Mp * D, 0.0f),
                       t((size_t)Mp * D), qkv((size_t)Mp * 3 * D), f((size_t)Mp * 4 * D);
    for (size_t s = 0; s < seqs.size(); s++)
        for (size_t p = 0; p < seqs[s].size(); p++) {
            float* xr = &x[(size_t)(start[s] + p) * D];
            const float* e = &wte_[(size_t)seqs[s][p] * D];
            const float* q = &wpe_[p * D];
            for (int d = 0; d < D; d++) xr[d] = e[d] + q[d];
        }

    const float scale = 1.0f / std::sqrt((float)hd);
    for (const Layer& L : layers_) {
        layer_norm(x.data(), h.data(), Mp, D, L.ln1_g, L.ln1_b, threads);
        matmul(h.data(), Mp, L.attn, qkv.data(), threads);
        // Causal self-attention, one task per (sequence, head).
        parallel_for(seqs.size() * H, threads, [&](size_t task){
            const int s = (int)(task / H), hh = (int)(task % H), r0 = start[s], T = (int)seqs[s].size();
            std::vector<float> w(T);
            for (int i = 0; i < T; i++) {
                const float* q = &qkv[(size_t)(r0 + i) * 3 * D + hh * hd];
                float mx = -INFINITY;
                for (int j = 0; j <= i; j++) {
                    const float* k = &qkv[(size_t)(r0 + j) * 3 * D + D + hh * hd];
                    float dot = 0;
                    for (int d = 0; d < hd; d++) dot += q[d] * k[d];
                    w[j] = dot * scale;
                    mx = std::max(mx, w[j]);
                }
                float sum = 0;
                for (int j = 0; j <= i; j++) { w[j] = std::exp(w[j] - mx); sum += w[j]; }
                float* o = &a[(size_t)(r0 + i) * D + hh * hd];
                for (int d = 0; d < hd; d++) o[d] = 0;
                for (int j = 0; j <= i; j++) {
                    const float* v = &qkv[(size_t)(r0 + j) * 3 * D + 2 * D + hh * hd];
                    const float wj = w[j] / sum;
                    for (int d = 0; d < hd; d++) o[d] += wj * v[d];
                }
            }
        });
        matmul(a.data(), Mp, L.proj, t.data(), threads);
        for (size_t k = 0; k < (size_t)M * D; k++) x[k] += t[k];
        layer_norm(x.data(), h.data(), Mp, D, L.ln2_g, L.ln2_b, threads);
        matmul(h.data(), Mp, L.fc, f.data(), threads);
        parallel_for((size_t)M, threads, [&](size_t r){          // GELU (tanh approximation)
            float* fr = &f[r * 4 * D];
            for (int d = 0; d < 4 * D; d++) {
                const float v = fr[d];
                fr[d] = 0.5f * v * (1.0f + std::tanh(0.7978845608028654f * (v + 0.044715f * v * v * v)));
            }
        });
        matmul(f.data(), Mp, L.out, t.data(), threads);
        for (size_t k = 0; k < (size_t)M * D; k++) x[k] += t[k];
    }
    layer_norm(x.data(), h.data(), Mp, D, lnf_g_, lnf_b_, threads);

    // Output layer: only positions followed by a token are needed.  The softmax over the
    // vocabulary is split by weight panel: each task keeps, per row, the panel's maximum
    // logit and sum of exponentials; they are combined per row in panel order.
    std::vector<int> row_of, target;
    for (size_t s = 0; s < seqs.size(); s++)
        for (size_t p = 0; p + 1 < seqs[s].size(); p++) { row_of.push_back(start[s] + (int)p); target.push_back(seqs[s][p + 1]); }
    const int R = (int)row_of.size(), Rp = (R + MR - 1) / MR * MR, P = head_.panels;
    std::vector<float> hr((size_t)Rp * D, 0.0f);
    for (int r = 0; r < R; r++) std::copy_n(&h[(size_t)row_of[r] * D], D, &hr[(size_t)r * D]);
    std::vector<float> pmax((size_t)Rp * P), psum((size_t)Rp * P), tlogit(Rp, 0.0f);
    const int row_blocks = (Rp + ROW_BLOCK - 1) / ROW_BLOCK;
    constexpr int PANEL_GROUP = 32;
    const int groups = (P + PANEL_GROUP - 1) / PANEL_GROUP;
    parallel_for((size_t)row_blocks * groups, threads, [&](size_t task){
        const int g = (int)(task % groups), r0 = (int)(task / groups) * ROW_BLOCK, r1 = std::min(Rp, r0 + ROW_BLOCK);
        float out[MR][NR];
        for (int p = g * PANEL_GROUP; p < std::min(P, (g + 1) * PANEL_GROUP); p++) {
            const float* panel = head_.data.data() + (size_t)p * D * NR;
            const int v0 = p * NR, cols = std::min(NR, n_vocab_ - v0);
            for (int i = r0; i < r1; i += MR) {
                micro_kernel(&hr[(size_t)i * D], D, panel, out);
                for (int r = 0; r < MR; r++) {
                    const int row = i + r;
                    float mx = -INFINITY;
                    for (int j = 0; j < cols; j++) mx = std::max(mx, out[r][j]);
                    float sum = 0;
                    for (int j = 0; j < cols; j++) sum += std::exp(out[r][j] - mx);
                    pmax[(size_t)row * P + p] = mx;
                    psum[(size_t)row * P + p] = sum;
                    if (row < R && target[row] >= v0 && target[row] < v0 + cols) tlogit[row] = out[r][target[row] - v0];
                }
            }
        }
    });
    std::vector<double> lp(seqs.size(), 0.0);
    size_t r = 0;
    for (size_t s = 0; s < seqs.size(); s++)
        for (size_t p = 0; p + 1 < seqs[s].size(); p++, r++) {
            double mx = -INFINITY;
            for (int q = 0; q < P; q++) mx = std::max(mx, (double)pmax[r * P + q]);
            double sum = 0;
            for (int q = 0; q < P; q++) sum += psum[r * P + q] * std::exp(pmax[r * P + q] - mx);
            lp[s] += tlogit[r] - mx - std::log(sum);
        }
    return lp;
}
