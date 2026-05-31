# Vigenère Cipher Breaker

A C++ tool for cracking ciphertexts encrypted with the Vigenère cipher, with no knowledge of the key. Works on ciphertexts from 5 characters upward, including cases where the ciphertext is nearly as short as the key itself.

---

## Table of Contents

1. [Background](#1-background)
2. [Theory](#2-theory)
3. [Cracking Methods](#3-cracking-methods)
4. [Modes of Operation](#4-modes-of-operation)
5. [Building and Usage](#5-building-and-usage)
6. [Output Format](#6-output-format)
7. [Performance Characteristics](#7-performance-characteristics)

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

### 1.2 Assumptions

- Key length is at most **12 characters**
- Plaintext consists of **lowercase English letters only** (spaces and punctuation stripped before encrypting)
- Plaintext is **natural English text** composed of recognizable words

---

## 2. Theory

### 2.1 Tetragram Fitness

The core scoring function measures how closely a candidate plaintext resembles English by computing the average log-probability of its four-letter sequences (tetragrams):

```
fitness(text) = (1/N) * Σ log P(text[i..i+3])
```

where `N` is the number of tetragrams and probabilities come from a pre-built table of observed English tetragram frequencies. English text typically scores around **−9 to −13**; random text scores below **−15**.

All hot-path fitness computations work directly on cipher/key bytes with no heap allocation.

### 2.2 Index of Coincidence (Key Length Detection)

The Index of Coincidence (IoC) of a text measures the probability that two randomly chosen characters are identical:

```
IoC = 26 * Σ n[i]*(n[i]-1) / (N*(N-1))
```

English text has IoC ≈ 1.7; uniformly random text has IoC ≈ 1.0. When the ciphertext is split into `m` slices (slice `j` = every `m`-th character starting at position `j`), each slice is a Caesar-shifted English text if `m` equals the true key length. The algorithm tries `m = 1, 2, 3, …` and returns the first `m` whose average slice IoC exceeds **1.6**.

### 2.3 Word Segmentation

Candidate plaintexts are evaluated not just by fitness but by how much of the text can be partitioned into recognized English words. The tool uses **dynamic programming** (longest-coverage segmentation) that, when coverage is tied, prefers longer individual words — so "sunshine" is returned as one word rather than "sun" + "shine".

Each segment is annotated with its frequency rank from the word list (rank 1 = most common). The average rank across all found words (lower = more common) is the `word_rank` metric.

### 2.4 Key Constraint Propagation

Several methods derive key values directly from a word hypothesis. If dictionary word `w` is hypothesized to appear at cipher position `x`:

```
k[(x+j) mod L] = (c[x+j] - w[j] + 26) mod 26,  for j = 0..len(w)-1
```

This simultaneously constrains multiple key slots. A hypothesis is **inconsistent** if two words imply different values for the same key slot and is pruned immediately.

---

## 3. Cracking Methods

### 3.1 Variational Hill-Climbing

**When used:** ciphertexts ≥ 30 characters, key length estimated by IoC.

Starting from a random key, repeatedly pick a random key position and try all 26 letters. Keep the change if it improves tetragram fitness. Runs `1000 × key_length` iterations per restart; multiple restarts with different seeds are run in parallel.

An incremental fitness calculation (tracking only the tetragrams affected by the changed key position) makes each trial O(ciphertext_length / key_length) instead of O(ciphertext_length), enabling thousands of trials per second.

### 3.2 Stochastic Short-Key Search

**When used:** key lengths 1–6, any ciphertext length.

Maintains a **pool** of the top-40 key candidates by fitness. Each iteration picks a random ciphertext position, derives key fragments from every dictionary word that fits there, and inserts the resulting key into the pool if it beats the current worst. After many iterations, all pool keys are validated by word coverage.

This is effective for short keys because each word hypothesis constrains a large fraction of the total key.

### 3.3 Long-Word Exhaustive Search

**When used:** key lengths 7–12.

For every dictionary word whose length ≥ key length, the key is fully determined in one consistency-check step (the word wraps around the key at least once, over-constraining it). If the derived key passes the fitness gate, the candidate plaintext is validated. Runs in parallel across dictionary words using OpenMP.

### 3.4 Word-Seed Hill-Climbing

**When used:** key lengths 7–12, dictionary words shorter than key length.

Each (word, position) pair seeds the key, then uncovered key positions are filled by one-position-at-a-time hill-climbing (two passes to let positions correct each other). Effectively combines the accuracy of word-hypothesis seeding with the flexibility of gradient search.

### 3.5 Word Cover Search (CSP Backtracking)

**When used:** exhaustive mode only, ciphertexts ≤ 25 characters.

A recursive constraint-satisfaction search that places dictionary words left-to-right, propagating key constraints at each step and backtracking on inconsistency. Unlike methods 3.3 and 3.4, it finds solutions where **multiple short words collectively reveal the key** with no single word spanning the full key length.

At the root level a minimum first-word length heuristic (`kl−1` by default) limits branching. All subsequent levels allow words from length 2 upward. The search is bounded by a node budget and a result limit per key length; both are configurable.

---

## 4. Modes of Operation

| Flag | Mode | Methods Used | Use Case |
|---|---|---|---|
| *(default)* | BOTH | long-key → short-key if no hit | General decryption |
| `--long-key` | LONG_FIRST | long-key; skip short-key if high coverage found | Known or suspected long key |
| `--short-key` | SHORT_ONLY | stochastic short-key only | Known short key |
| `--exhaustive` | EXHAUSTIVE | all methods, all key lengths, no early exit | Short ciphertexts, uncertain key length |

**Default mode** runs the long-key search first (methods 3.3 + 3.4) for key lengths 7–12, then the short-key stochastic search (method 3.2) for key lengths 1–6. If the long-key phase produces a high-coverage result and `--long-key` is specified, the short-key phase is skipped.

**Exhaustive mode** tries every key length from 1 to 12 with no early exits, collects all candidates across all methods, and ranks them by: coverage ↓ → key length ↑ → word count ↑ → word rank ↑. A 90% coverage gate filters noise.

---

## 5. Building and Usage

### 5.1 Requirements

- GCC or Clang with C++17 support
- OpenMP (usually included with GCC)
- Python 3 (for the test suite only)

### 5.2 Building

```bash
make          # optimized build → bin/vigenere
make clean    # remove binary
```

### 5.3 Encrypting

```bash
bin/vigenere -e <plaintext_file>
```

The binary prompts for a key on stderr, normalizes both plaintext and key to lowercase letters, and writes the ciphertext to stdout.

```bash
echo "secret" | bin/vigenere -e plain.txt > cipher.txt
```

### 5.4 Decrypting (Cracking)

```bash
bin/vigenere -d <ciphertext_file> [options]
```

**Options:**

```
--long-key          run long-key search; skip short-key if high coverage found
--short-key         run short-key stochastic search only
--exhaustive        try all key lengths 1..12, no early exits

word_cover_search tuning (only active with --exhaustive):
  --wcs-min-word N  minimum first-word length at root  (default: kl-1; 0 = no limit)
  --wcs-results N   max results per key length          (default: 500;   0 = unlimited)
  --wcs-budget N    max recursion nodes per key length  (default: 500000; 0 = unlimited)
```

**Examples:**

```bash
# Standard crack — auto-selects method
bin/vigenere -d cipher.txt

# Short or ambiguous ciphertext — try everything
bin/vigenere -d cipher.txt --exhaustive

# Suspected long key only
bin/vigenere -d cipher.txt --long-key

# Short ciphertext, relax word-cover-search to explore more
bin/vigenere -d cipher.txt --exhaustive --wcs-min-word 3 --wcs-results 2000
```

### 5.5 Running Tests

```bash
bash tests/run_tests.sh
```

Tests cover key lengths 1–12 and ciphertext lengths from 5 to 24 characters. Each test encrypts a known plaintext, cracks it, and checks that the original plaintext appears among the candidates with ≥ 80% word coverage.

---

## 6. Output Format

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

Candidates are sorted: **coverage ↓ → key length ↑ → word count ↑ → word rank ↑**. The top candidate is the most likely correct decryption.

False positives are expected — especially in exhaustive mode on very short ciphertexts — and are by design. The ranked list lets a human (or downstream LLM) quickly identify the semantically meaningful candidate.

---

## 7. Performance Characteristics

| Ciphertext length | Key length | Mode | Typical time |
|---|---|---|---|
| ≥ 30 chars | any | default | < 5 s |
| 13–29 chars | 1–6 | default | < 10 s |
| 13–29 chars | 7–12 | `--long-key` | < 10 s |
| 8–12 chars | any | `--exhaustive` | 5–60 s |
| 5–7 chars | any | `--exhaustive` | < 5 s |

The word cover search budget (`--wcs-budget`) is the main lever for trading speed against recall on very short ciphertexts. The default of 500,000 nodes is sufficient to recover most single-phrase plaintexts; pass `0` for unlimited if you are willing to wait.

---

## Data Sources

- Tetragram frequencies: [practicalcryptography.com](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/)
- Word list: [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt)
