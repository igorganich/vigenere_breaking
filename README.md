# Vigenère Cipher Breaker

A C++ tool for cracking ciphertexts encrypted with the Vigenère cipher. Implements two cryptanalysis methods: a **variational method** for standard-length ciphertexts and a **dictionary-based variational method** for very short ones.

---

## Table of Contents

1. [Background](#1-background)
2. [Theory](#2-theory)
3. [Cracking Methods](#3-cracking-methods)
4. [Building and Usage](#4-building-and-usage)
5. [Known Limitations and Future Work](#5-known-limitations-and-future-work)

---

## 1. Background

### 1.1 The Vigenère Cipher

The Vigenère cipher is a periodic polyalphabetic substitution cipher. Each plaintext letter is shifted by an amount determined by the corresponding letter of a repeating key.

Formally, if the key has length `L` with characters `k[0], k[1], ..., k[L-1]`, then the `i`-th ciphertext character is:

```
c[i] = (p[i] + k[i mod L]) mod 26
```

where letters are represented as integers `A=0, B=1, ..., Z=25`.

Decryption reverses this:

```
p[i] = (c[i] - k[i mod L] + 26) mod 26
```

### 1.2 Assumptions

This tool operates under the following constraints:

- The encryption key is at most **12 characters** long
- The plaintext consists only of **lowercase English letters** (no spaces, digits, or punctuation)
- The plaintext is **natural English text**

---

## 2. Theory

### 2.1 Fitness: Measuring How English a Text Looks

The core of both cracking methods is a **fitness function** — a way to score how closely a candidate plaintext resembles natural English.

This implementation uses **tetragrams** (four-letter sequences) and their observed frequencies in English. The fitness of a text is the average log-probability of its tetragrams:

```python
def fitness(text):
    result = 0
    for i in range(len(text) - 3):
        tetragram = text[i:i+4]
        x = (index(tetragram[0]) * 26^3 +
             index(tetragram[1]) * 26^2 +
             index(tetragram[2]) * 26 +
             index(tetragram[3]))
        y = tetrafrequencies[x]
        result += log(y) if y > 0 else -15
    return result / (len(text) - 3)
```

Dividing by the number of tetragrams normalizes the score so it is independent of text length, making it comparable across candidate decryptions of different lengths. Typical English text scores around **-9.6**.

Tetragram frequency data is sourced from [practicalcryptography.com](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/).

### 2.2 Finding the Key Length: Index of Coincidence

The **Index of Coincidence (IoC)** measures the probability that two randomly chosen characters in a text are identical:

```
IoC = 26 * Σ(n[i] * (n[i] - 1)) / (N * (N - 1))
```

where `n[i]` is the count of letter `i` and `N` is the total character count. The factor of 26 normalizes the result so that:
- **Random text** → IoC ≈ 1.0
- **English text** → IoC ≈ 1.7

To find the key length, the ciphertext is split into `m` slices, where slice `j` contains every `m`-th character starting at position `j`. The average IoC across all slices is computed for increasing values of `m`. The smallest `m` whose average IoC exceeds **1.6** is taken as the key length.

```python
def find_key_length(ciphertext):
    period = 0
    while True:
        period += 1
        slices = [''] * period
        for i, char in enumerate(ciphertext):
            slices[i % period] += char
        avg_ioc = sum(index_of_coincidence(s) for s in slices) / period
        if avg_ioc > 1.6:
            return period
```

### 2.3 Dictionary-Based English Validation

For very short ciphertexts, statistical methods like IoC are unreliable due to insufficient data. An alternative validation method checks whether a candidate plaintext is composed primarily of English words.

The algorithm scans the candidate text for every word in a dictionary, marks each matched character, and computes the coverage ratio:

```python
def check_if_words(text):
    charmap = [False] * len(text)
    marked = 0
    for word in dictionary:
        pos = 0
        while (found := text.find(word, pos)) != -1:
            for i in range(len(word)):
                if not charmap[found + i]:
                    charmap[found + i] = True
                    marked += 1
            pos = found + 1
    return marked / len(text) > 0.95
```

If more than **95%** of characters are covered by dictionary words, the text is considered valid English.

Dictionary source: [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt)

---

## 3. Cracking Methods

### 3.1 Variational Method

This method works well for ciphertexts of moderate length (roughly 30+ characters, depending on key length). It is a **stochastic hill-climbing** search over the key space, guided by the fitness function.

The algorithm iterates many times. Each iteration picks a random key position, tries all 26 possible letters at that position, and keeps the change if it improves fitness:

```python
def variational_crack(ciphertext, key_length):
    key = ['A'] * key_length
    best_fitness = -99
    for _ in range(1000 * key_length):
        K = key[:]
        pos = randrange(key_length)
        for letter in ALPHABET:
            K[pos] = letter
            f = fitness(decrypt(ciphertext, K))
            if f > best_fitness:
                key = K[:]
                best_fitness = f
    return decrypt(ciphertext, key)
```

The `1000 * key_length` iteration count ensures each key position is revisited many times, allowing the search to escape local optima. The result is validated with `check_if_words`.

### 3.2 Dictionary-Based Variational Method

For very short ciphertexts (roughly 13–30 characters), the variational method can fail because statistical fitness scores are noisy with little data. This method takes a more aggressive approach: instead of mutating one key letter at a time, it uses **entire dictionary words as key update candidates**.

**Core insight:** if a dictionary word appears at ciphertext position `x`, then the key bytes at those positions are fully determined:

```
k[(x + j) mod L] = (c[x + j] - word[j] + 26) mod 26
```

for each letter `j` of the word. This is `get_key_from_word`.

The algorithm picks a random ciphertext position, applies this derivation for every word in the dictionary, and keeps the key update if it improves fitness:

```python
def dictionary_variational_crack(ciphertext, key_length):
    key = ['A'] * key_length
    best_fitness = -99
    for _ in range(1000):
        x = randrange(key_length)
        for word in dictionary:
            K = key[:]
            derived = get_key_from_word(ciphertext, word, x)
            insert_key_fragment(K, derived, x)
            f = fitness(decrypt(ciphertext, K))
            if f > best_fitness:
                key = K[:]
                best_fitness = f
    return decrypt(ciphertext, key)
```

Each dictionary word simultaneously updates multiple key positions, making each iteration a much larger jump through key space compared to the single-letter variational method. This helps escape the sparse fitness landscape that short ciphertexts produce.

**Trade-off:** this method is significantly slower than the variational method (potentially hours for short texts) due to the full dictionary iteration at each step.

---

## 4. Building and Usage

### 4.1 Building

Standard build:
```bash
cd src
make
```

Debug build (with debug symbols):
```bash
cd src
make debug
```

Both targets place the binary at `<PROJECT_ROOT>/bin/vigenere`.

Clean build artifacts:
```bash
cd src
make clean
```

### 4.2 Encryption

Encrypt a file with a key you provide interactively:
```bash
bin/vigenere -e plaintext_file > encrypted_file
```

The program normalizes the key to lowercase letters only before encrypting. The encrypted output is written to stdout.

### 4.3 Decryption (Cracking)

Crack an encrypted file without knowing the key:
```bash
bin/vigenere -d encrypted_file
```

The program automatically selects the appropriate cracking method:
- **Long ciphertext** → variational method (seconds)
- **Short ciphertext** → dictionary-based variational method (potentially hours)

Output is the recovered plaintext followed by the discovered key on the next line.

### 4.4 Verbose Mode

To monitor progress during cracking, set the `VERBOSE` environment variable:
```bash
export VERBOSE=1
bin/vigenere -d encrypted_file
```

In verbose mode, the program prints candidate plaintexts and the current completion percentage to stderr.

### 4.5 Running Tests

```bash
cd src
make test
```

This runs the bundled test suite. Note that test cases involving short ciphertexts will run slowly due to the dictionary-based method.

---

## 5. Known Limitations and Future Work

**False positives on short texts.** The `check_if_words` function with a 0.95 threshold still produces false positives for very short plaintexts, where random letter combinations can accidentally match many short dictionary words. Possible improvements:
- Ignore very short words (e.g. under 3 characters) that match too broadly
- Weight matches by word frequency
- Explore a knapsack-style covering approach for more precise coverage scoring

**False negatives.** The single best-key approach means an unlucky random walk can miss the correct key. Maintaining a pool of the top-N key candidates instead of only the single best would reduce false negatives.

**Performance.** The dictionary-based method is inherently slow due to full dictionary iteration at each step. Possible optimizations:
- Parallelize across CPU cores
- Use a trie to prune impossible words early based on partial key constraints
- Precompute and index key derivations

**Key length detection for short texts.** IoC-based key length detection becomes unreliable below ~30 characters. For very short ciphertexts, trying all key lengths from 1 to 12 in parallel and using fitness as the discriminator may be more robust.
