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

### 2.2 Word Language Model and Segmentation

Word probabilities are estimated from frequency rank with Zipf's law, `P(rank r) = (1/r) / Σ 1/r'`. Each candidate plaintext is split into the **most probable sequence of words** by dynamic programming; letters not covered by any word ("stray") cost −15 each, less than any word, so coverage is maximised first. The resulting log-probability is the candidate's `lm`.

### 2.3 Ranking

A candidate is a (key, plaintext) pair, scored by its posterior log-probability:

```
score = lm(plain) − key_len · ln 26
```

The second term is the prior of a random key: there are 26^kl keys of length kl, so a fluent plaintext reachable with a short key is stronger evidence than equally fluent text from a long one. Candidates are sorted by score, after dropping those below **90% word coverage**.

On the benchmark this ranks the true plaintext first in 34 of the 36 cases where it was found (the previous coverage → key length → word count → average-rank order managed 26), and 17 of 18 on a separate set of natural phrases.

### 2.4 Tetragram Fitness

Each candidate is also reported with the average log-probability of its four-letter sequences. English typically scores −9 to −13, random text below −15. Fitness is informational only.

---

## 3. The Search

For every key length `kl = 1 .. min(12, len−1)`, a recursive search covers the ciphertext with words, left to right:

1. Place a first word at position 0. First words are tried in frequency order.
2. At the leftmost uncovered position, walk the **dictionary trie** along the ciphertext. Where the key slot for a position is already fixed, the plaintext letter is known and only that child is followed; where it is free, every child is tried and fixes the slot. Contradictory words are never generated.
3. At each word end, recurse to the next position.
4. As soon as **every key slot is fixed**, the rest of the plaintext is determined: decrypt once, keep the candidate if its word coverage is ≥ 90%, and stop branching.

Candidates go into one pool that keeps the best `--top` (default 10,000) distinct plaintexts by score, so memory stays bounded however long the search runs. The same plaintext reached with several keys — `abc` and its repeat `abcabc` — is kept once, with the shorter key.

Because several short words jointly pin down the key, no single word has to span the whole key length.

### 3.1 Anytime order and time limit

The search is split into buckets, one per (key length, first-word length), and the buckets run from cheapest to most expensive:

- **Stage 0:** for every key length, first words of length ≥ `kl − 1`, longest first. Such a word fixes nearly the whole key, so this stage takes seconds.
- **Stage s:** for every key length, first words of length `kl − 1 − s`, down to `--wcs-min-word` (default 1, so "a" and "i" can start the plaintext).

Shorter first words fix fewer key slots and branch far more, so each stage costs more than the last. When the **time limit** (default 300 s, calibration included) expires, the search stops and prints everything found so far; the coverage report (§3.2) says how much of the full search that was.

**Per-bucket bounds:**

| Bound | Default | Effect |
|---|---|---|
| Results | 500 | Full coverings kept per bucket; the bucket stops when this is reached. Near-misses (90–99% coverage) have a separate cap of the same size that never stops the search. |
| Node budget | 500,000 | Word placements per bucket. |

Each bucket has its own bounds, so buckets never crowd each other out: the run order only changes *when* results appear, not which.

Buckets are independent, so they run in parallel: worker threads (`--threads`, default all hardware threads) take buckets in the order above, each with its own search state and candidate pool, merged at the end. The output is identical to a single-threaded run. On a 20-thread machine this is about 3× faster on 10-letter texts and 9× on 19-letter ones; the speedup is limited by the few largest buckets, which cannot be split.

### 3.2 Calibration and the coverage report

Coverage is measured against the **full-vocabulary search**: every first word of every length, every consistent continuation, no caps (`--time-limit 0 --wcs-budget 0 --wcs-results 0`).

Before searching, the tool **calibrates** every bucket on the current machine: it times a uniform random sample of up to 16 first words with caps off (at most 0.05 s per bucket, and 10% of the time limit in total) and extrapolates, in the manner of Knuth's estimator for backtracking searches. First words are searched in frequency order, so the time a partly searched bucket has taken says little about its remaining, rarer words; the random sample does.

After the search, a short **coverage report** on stderr says how much of the full-vocabulary search this run did and what a full run would take:

```
coverage: this run searched <47% of the full vocabulary (estimated by sampling)
for full coverage: --time-limit 60 --wcs-budget 0 --wcs-results 0  (est. at least 25.2s with 20 threads; raise the limit if it is reached)
```

`--report buckets` adds a table of the buckets that did not finish (`--report all`: every bucket; `--report none`: nothing):

| Column | Meaning |
|---|---|
| `status` | Why the bucket stopped: `complete`, `time limit`, `node budget`, `result cap`, or `not run` (time ran out first) |
| `first words` | First words fully searched / words of that length |
| `word prob.` | Share of the bucket's first-word probability searched: the chance, under the word model, that the true first word was tried |
| `cpu time` | Thread time spent on the bucket |
| `est. to full` | Estimated thread time to search the rest of the bucket with no caps |

**The estimates are loose lower bounds** (`<` / `at least`): calibration only sees the start of the expensive subtrees. For `helloworld` a 60 s run said "at least 1m04s" and the full search took 10 min on 20 threads. The suggested `--time-limit` is 1.5× the estimate; if the limit is reached, raise it and run again.

For long full runs, `--progress S` prints the current bucket and its progress on stderr every S seconds.

---|---|
| `status` | Why the bucket stopped: `complete`, `time limit`, `node budget`, `result cap`, or `not run` (time ran out first) |
| `first words` | First words searched / words of that length, always the most common ones |
| `word prob.` | Share of the bucket's first-word probability searched — the chance, under the word model, that the true first word was tried |
| `est. left` | Estimated time to finish the bucket under current caps (0 once it stopped at a cap) |
| `est. no caps` | Estimated time to search the whole bucket with no caps |

The two summary lines show the gap between this run and a complete one. In the example, the complete search was estimated at over 1 min 36 s and actually took 1 min 44 s. `>` marks a lower bound: a sample was cut short, so the true cost may be higher.

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
--time-limit S    stop after S seconds and print what was found (default: 300; 0 = none)
--top N           print the N most probable candidates (default: 10000; 0 = all)
--threads N       worker threads (default: 0 = all hardware threads)
--progress S      print search progress on stderr every S seconds (default: off)
--report MODE     coverage report on stderr: summary, buckets (unfinished buckets too),
                  all (every bucket), or none (default: summary)
--wcs-min-word N  shortest first word to try, never above kl-1 (default: 1)
--wcs-results N   max results per key length and first-word length (default: 500; 0 = unlimited)
--wcs-budget N    max word placements per key length and first-word length (default: 500000; 0 = unlimited)
```

**Examples:**

```bash
# Standard crack (at most ~5 min)
bin/vigenere -d cipher.txt

# Search the entire vocabulary: the coverage report prints the exact flags
# and an estimated duration for the ciphertext at hand
bin/vigenere -d cipher.txt --time-limit 600 --wcs-budget 0 --wcs-results 0

# Quick look: first word of at least kl−1 letters only (≤ 10 s)
bin/vigenere -d cipher.txt --wcs-min-word 99
```

### 4.5 Tests and Benchmark

```bash
bash tests/run_tests.sh                 # fixed cases, key lengths 1–12, 5–24 letters
python3 bench/bench.py                  # randomized grid; results in bench/results.csv
python3 bench/bench.py --methods csp-strict,csp-t30   # compare settings
python3 bench/bench.py --summary        # re-print the summary from the CSV
```

The benchmark generates plaintexts from common English words (8–24 letters) with random keys (2–12 letters) and records the rank of the true plaintext, candidate count and time. `--bin` benchmarks a different binary.

---

## 5. Output Format

Each candidate is printed as a 5-line block:

```
--- candidate 1 ---
key:      network
plain:    algorithmdesign
words:    [algorithm:7551][design:690]
coverage: 100.0%  key_len: 7  words: 2  word_rank: 4120  fitness: -11.09  lm: -20.31  score: -43.12
```

| Field | Meaning |
|---|---|
| `key` | Recovered key |
| `plain` | Decrypted plaintext |
| `words` | Most probable word segmentation with per-word frequency ranks |
| `coverage` | Fraction of plaintext characters covered by words |
| `key_len` | Key length |
| `words` (stat) | Number of words in the segmentation |
| `word_rank` | Average frequency rank of those words (lower = more common) |
| `fitness` | Average tetragram log-probability (higher = more English-like) |
| `lm` | Log-probability of the segmentation under the word model (§2.2) |
| `score` | Ranking score: `lm − key_len · ln 26` (§2.3) |

The top candidate is the most probable decryption. A very short ciphertext genuinely admits many readings, so the list is long; the ranking puts the plausible ones first.

---

## 6. Performance and Limitations

Benchmark: 78 random cases (seed 1, 3 per (length, key length) cell), single-threaded.

> These numbers predate multithreading, the bounded candidate pool, the separate near-miss cap and one-letter first words; they have not been re-measured yet.

| Setting | Found | Top-1 | Top-10 | Median time |
|---|---|---|---|---|
| `--wcs-min-word 99` (stage 0 only) | 46.2% | 43.6% | 46.2% | 3 s |
| `--time-limit 10` | 53.8% | 50.0% | 52.6% | 10 s |
| `--time-limit 30` | 56.4% | 52.6% | 55.1% | 30 s |
| **default** (`--time-limit 60`) | **60.3%** | **53.8%** | **59.0%** | 60 s |
| `--time-limit 0` (complete) | 66.7% | 60.3% | 65.4% | ~140 s, up to ~10 min |

Found rate by key length (default settings): 100% at `kl = 2` and 4, 67% at 6, 33% at 8, 8% at 10, 22% at 12.

**Known limitations:**

- **Long keys.** With a key of 8–12 letters, a ciphertext of 8–24 letters constrains each key slot only once or twice, so thousands of readings fit and the relaxed stages for long keys come last. Even the complete search finds only 17–50% of these cases.
- **Run time grows with length.** The complete search takes seconds at 8–12 letters but minutes at 16–24; the default time limit trades the last ~6 points of recall for a bounded run.
- **Word model.** Word probabilities are estimated from rank only (no counts, no word pairs), and the benchmark's plaintexts come from the same ranked list, which flatters the model. A word-bigram model would be the next step for ranking.

---

## Data Sources

- Tetragram frequencies: [practicalcryptography.com](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/)
- Word list: [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt)
