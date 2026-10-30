# Vigenère Cipher Breaker

A C++ tool for cracking **ultra-short** Vigenère ciphertexts (4–25 letters) with no knowledge of the key, including cases where the ciphertext is barely longer than the key itself. It uses one method: a constraint-satisfaction search that covers the ciphertext with dictionary words and derives the key from them.

---

## Table of Contents

1. [Background](#1-background)
2. [Theory](#2-theory)
3. [The Search](#3-the-search)
4. [Building and Usage](#4-building-and-usage)
5. [Output Format](#5-output-format)
6. [Performance and Limitations](#6-performance-and-limitations)

---

## 1. Background

### 1.1 The Vigenère Cipher

The Vigenère cipher is a periodic polyalphabetic substitution cipher. Each plaintext letter is shifted cyclically by the corresponding letter of a repeating key.

Formally, with key length `L`:

```
c[i] = (p[i] + k[i mod L]) mod 26
p[i] = (c[i] - k[i mod L] + 26) mod 26
```

where letters are integers `a=0, b=1, ..., z=25`.

### 1.2 Why short ciphertexts are hard

Classic attacks (index of coincidence, frequency analysis, tetragram hill-climbing) need enough text per key position to see English statistics. At 8–25 letters with a key of up to 12, each key position sees only one to a few letters, so statistics say almost nothing. What remains is the fact that the plaintext must be made of real words — this tool searches on that constraint directly.

### 1.3 Assumptions

- Key length is at most **12 characters**
- Ciphertext is **4–25 letters** (longer input is rejected)
- Plaintext consists of **lowercase English letters only** (spaces and punctuation stripped before encrypting)
- Plaintext is **natural English text** composed of dictionary words (plus "a" and "i", which the search adds to the word list)

---

## 2. Theory

### 2.1 Key Constraint Propagation

If dictionary word `w` is hypothesized to appear at cipher position `x`:

```
k[(x+j) mod L] = (c[x+j] - w[j] + 26) mod 26,  for j = 0..len(w)-1
```

Each word fixes several key slots at once. A hypothesis is **inconsistent** if two words imply different values for the same key slot and is pruned immediately.

### 2.2 Word Segmentation

Each candidate plaintext is partitioned into dictionary words using **dynamic programming** (longest-coverage segmentation) that, when coverage is tied, prefers longer individual words — so "sunshine" is returned as one word rather than "sun" + "shine".

Each segment is annotated with its frequency rank from the word list (rank 1 = most common). The average rank across all found words (lower = more common) is the `word_rank` metric.

### 2.3 Tetragram Fitness

Each candidate is also reported with the average log-probability of its four-letter sequences:

```
fitness(text) = (1/N) * Σ log P(text[i..i+3])
```

English text typically scores around **−9 to −13**; random text below **−15**. Fitness is informational only — it does not affect ranking.

---

## 3. The Search

For every key length `kl = 1 .. min(12, len−1)`, a recursive search places dictionary words left to right:

1. Find the leftmost ciphertext position not yet covered by a word.
2. Try each dictionary word that fits there, shortest first (one-letter "a"/"i" allowed after the first word). Derive the key slots it implies (§2.1); skip it if they contradict slots already fixed.
3. Recurse. When every position is covered, the key is fully determined and the decryption is recorded as a candidate.
4. Undo the word's key slots and try the next one.

Because several short words jointly pin down the key, no single word has to span the whole key length.

**Bounds** (all configurable, see §4.3):

| Bound | Default | Effect |
|---|---|---|
| First-word minimum length | `kl − 1` | Limits branching at the root. The first word of the plaintext must be at least this long. |
| Results per key length and first-word length | 500 | Each first-word length gets its own cap, so coverings that start with one length cannot crowd out the others. |
| Node budget per key length | 500,000 | Search for this `kl` stops after this many recursive calls. |

Afterwards, candidates are deduplicated by key, segmented, filtered to **≥ 90% word coverage**, and sorted by **coverage ↓ → key length ↑ → word count ↑ → word rank ↑**.

---

## 4. Building and Usage

### 4.1 Requirements

- GCC or Clang with C++17 support
- Python 3 (for the test suite and benchmark only)

### 4.2 Building

```bash
make          # optimized build → bin/vigenere
make clean    # remove binary
make test     # run the test suite
```

### 4.3 Encrypting

```bash
bin/vigenere -e <plaintext_file>
```

The binary prompts for a key on stderr, normalizes both plaintext and key to lowercase letters, and writes the ciphertext to stdout.

```bash
echo "secret" | bin/vigenere -e plain.txt > cipher.txt
```

### 4.4 Decrypting (Cracking)

```bash
bin/vigenere -d <ciphertext_file> [options]
```

**Options:**

```
--wcs-min-word N  minimum first-word length at root  (default: kl-1; 0 = no limit)
--wcs-results N   max results per key length and first-word length (default: 500; 0 = unlimited)
--wcs-budget N    max recursion nodes per key length  (default: 500000; 0 = unlimited)
```

**Examples:**

```bash
# Standard crack
bin/vigenere -d cipher.txt

# Very short text with a long key: don't cap the number of coverings
bin/vigenere -d cipher.txt --wcs-results 0
```

Lowering `--wcs-min-word` lets the plaintext start with a short word, but the search grows very quickly — with `2`, most texts of 12+ letters do not finish in 2 minutes.

### 4.5 Tests and Benchmark

```bash
bash tests/run_tests.sh          # fixed cases, key lengths 1–12, 5–24 letters
python3 bench/bench.py           # randomized grid; results in bench/results.csv
python3 bench/bench.py --summary # re-print the summary from the CSV
```

The benchmark generates plaintexts from common English words (8–24 letters) with random keys (2–12 letters) and records the rank of the true plaintext, candidate count and time.

---

## 5. Output Format

Each candidate is printed as a 5-line block:

```
--- candidate 1 ---
key:      network
plain:    algorithmdesign
words:    [algorithm:1847][design:1502]
coverage: 100.0%  key_len: 7  words: 2  word_rank: 1675  fitness: -10.34
```

| Field | Meaning |
|---|---|
| `key` | Recovered key |
| `plain` | Decrypted plaintext |
| `words` | Non-overlapping word segmentation with per-word frequency ranks |
| `coverage` | Fraction of plaintext characters covered by recognized words |
| `key_len` | Key length (matches `key` string length) |
| `words` (stat) | Number of non-overlapping dictionary words found |
| `word_rank` | Average frequency rank of found words (lower = more common) |
| `fitness` | Average tetragram log-probability (higher = more English-like) |

The top candidate is the most likely decryption. False positives are expected — a very short ciphertext genuinely admits many readings. The ranked list lets a human (or downstream LLM) pick the semantically meaningful one.

---

## 6. Performance and Limitations

Benchmark: 78 random cases (seed 1, 3 per cell), default settings, single-threaded, no timeout.

| Ciphertext length | Typical time | Found | Top-10 |
|---|---|---|---|
| 8 letters | < 5 s | 78% | 44% |
| 12 letters | ~8 s | 47% | 40% |
| 16 letters | ~50 s | 44% | 39% |
| 20 letters | ~100 s | 44% | 44% |
| 24 letters | ~120 s | 33% | 33% |

Time depends on ciphertext length, not key length. When the true plaintext is found it is ranked #1 about three times out of four; mis-rankings concentrate in 8–12 letter texts, which produce 1,500–2,600 candidates.

**Known limitations:**

- **First-word rule.** Every miss in the benchmark was a plaintext whose first word is shorter than `kl − 1` (36/36 reachable cases found, 0/42 blocked ones). This is why success drops with key length: 100% at `kl = 2`, 27% at 6, 0% at 12.
- **Node budget does not bound time.** It counts recursive calls, but each call scans the whole dictionary, so a run can take far longer than the budget suggests.

---

## Data Sources

- Tetragram frequencies: [practicalcryptography.com](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/)
- Word list: [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt)
