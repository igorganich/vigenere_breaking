# Vigenère Cipher Breaker

A C++ tool that breaks **ultra-short** Vigenère ciphertexts (2–25 letters) with no knowledge of the key. For every key length from 1 to 12 it finds **every** key whose plaintext splits completely into dictionary words, ranks all of them by probability, and stores them in an index file you can query by rank: the best 10, the millionth, or any range.

---

## Table of Contents

1. [Background](#1-background)
2. [How the Search Works](#2-how-the-search-works)
3. [Ranking](#3-ranking)
4. [Building and Usage](#4-building-and-usage)
5. [The Index File](#5-the-index-file)
6. [Performance](#6-performance)
7. [Limitations](#7-limitations)
8. [Tests and Benchmark](#8-tests-and-benchmark)

---

## 1. Background

### 1.1 The Vigenère cipher

Each plaintext letter is shifted by the corresponding letter of a repeating key. With key length `L` and letters as numbers `a=0 … z=25`:

```
c[i] = (p[i] + k[i mod L]) mod 26
p[i] = (c[i] - k[i mod L] + 26) mod 26
```

### 1.2 Why short ciphertexts are hard

Classic attacks (index of coincidence, frequency analysis, n-gram hill-climbing) need enough text per key letter to see English statistics. With 8–25 letters and a key of up to 12, each key letter decodes only one to three plaintext letters, so statistics say almost nothing. What remains is the constraint that the plaintext must be made of real words. This tool searches on that constraint directly and exhaustively.

### 1.3 Assumptions

- Ciphertext of **2–25 letters** (non-letters are stripped; longer input is rejected).
- Key length **1–12**.
- The plaintext consists **entirely of dictionary words**: the bundled word list of 44,275 words plus "a" and "i". A plaintext containing a word outside the list is not found (see [§7](#7-limitations)).

---

## 2. How the Search Works

### 2.1 In simple words

Write the plaintext in rows of `L` letters, one row per repetition of the key:

```
row 1:  i a m t o l d t o g o a     ← positions 1–12
row 2:  n d s e e i t b y f o u     ← positions 13–24
row 3:  r                           ← position 25
```

Each **column** belongs to one key letter, and choosing that key letter fills in the whole column at once. So the search goes column by column, trying all 26 key letters, and in each row keeps reading as if spelling words: is what this row has read so far still the start of some word, or a finished word? If any row stops making sense, that choice dies.

After a few columns, all that matters for the rest of the search is a small **bookmark** per row: "row 1 is three letters into a word starting with *to…*", "row 2 just finished a word". *Which* key letters led there no longer matters: those columns are done and never read again. Huge numbers of different key prefixes lead to the same bookmarks, and the search merges them and continues from each bookmark once. That is what makes an exhaustive search feasible: for 25 letters and key length 12 the number of distinct bookmark combinations peaks around 30 million, against roughly 10¹⁷ possible keys.

### 2.2 Details

This is a dynamic program over key columns (`src/dp.cpp`):

- **State per row (track):** either *inside a word that started in this row*, as a node of the dictionary trie (the root means "at a word boundary"); or *reading the tail of a word that started in an earlier row*, as a node of a trie of all proper word suffixes. Each column's states are deduplicated.
- **Words across row boundaries:** a row may start in the middle of a word. Its state remembers the tail it read; once all columns are done, the head left open at the end of the previous row plus that tail must form a dictionary word (a word may span several rows).
- **Backward pass:** states that cannot reach an accepted end are discarded.
- **Reading off keys:** the columns are walked again, carrying for each key prefix the *set* of states it can be in, so every key comes out exactly once.
- **Short keys (1–4)** would split the text into up to 25 rows, but their key space is tiny (≤ 26⁴ = 456,976 keys), so they are searched by brute force.
- **Repeated keys:** a key like `abcabc` gives the same plaintext as `abc`; only the shortest form is kept, so every plaintext appears once.
- Work is spread over all CPU threads.

### 2.3 Key lengths that are skipped

Two cases carry no usable information, and `crack` skips the key length with a note rather than enumerating English:

- **The ciphertext repeats with period `L`.** Then the plaintext must repeat too, and any repeating English text is an equally good answer.
- **More candidates than `--max-candidates`** (default 150 million). This happens when the key is nearly as long as the text: for key length 9 on 10 letters, only one plaintext letter is tied to another, so almost any word sequence fits.

---

## 3. Ranking

Candidates are ranked by their posterior log-probability:

```
score = log P(plaintext) − key_length · ln 26
```

- **log P(plaintext)** is the most probable split into words under a language model, chosen with `--model`:
  - **`trigram`** (the default): see below.
  - **`unigram`**: single words, `P(w) = c(w) / Σ c` from real word counts (`txt/english_unigrams.txt`).
  - **`bigram`**: word pairs. `P(w₂ | w₁) = c(w₁ w₂) / c(w₁)` for the 235,000 pairs in `txt/english_bigrams.txt`, and `α(w₁) · P(w₂)` for every other pair, where `α(w₁)` spreads the probability the listed pairs leave over, so `P(· | w₁)` sums to 1. The table only holds pairs seen at least 100,000 times in its corpus, so every unlisted pair is known to be rarer; nothing is tuned. The first word uses `P(w)`. The split is found by a DP whose state is the last word.
  - **`trigram`**: word triples, backing off to `bigram`. `P(w₃ | w₁ w₂) = (c(w₁ w₂ w₃) − D) / c(w₁ w₂)` for the 978,000 triples in `txt/english_trigrams.txt` (seen at least 15 times in 179 million words of film subtitles), and `α(w₁ w₂) · P(w₃ | w₂)` for every other word, with `α` again making each context sum to 1. `c(w₁ w₂)` counts every continuation of the pair, including triples below the cutoff (`txt/english_trigram_contexts.txt`); `D = n₁ / (n₁ + 2n₂) ≈ 0.53` is the usual absolute discount, computed from the counts. The DP state is the last two words. The second word of a text uses `P(w₂ | w₁)`.
  
  On the realistic benchmark (`bench/realistic.py`, 84 of its 105 phrases measured) `trigram` ranks 95% of the true plaintexts first and `bigram` 80% (mean reciprocal rank 0.956 against 0.839); the two found the same 82 plaintexts and the other two have their key lengths skipped by the search. Of the 15 phrases where they disagree, `trigram` ranks 14 higher (e.g. "out of sight out of mind": 1 against 40). An earlier `zipf` model (single words, probabilities from frequency rank) was removed: it ranked 9 of the first 46 phrases below first place where `trigram` ranked all of them first, and the stress case at 4,980 against 9.
- **− key_length · ln 26** is the prior of a random key: there are 26^L keys of length L, so a fluent plaintext from a short key is stronger evidence than the same from a long one.

Ties are broken by key, so the order is deterministic.

---

## 4. Building and Usage

### 4.1 Requirements

- GCC or Clang with C++17 support
- Python 3 (benchmark only)

### 4.2 Building

```bash
make          # optimized build → bin/vigenere
make test     # unit tests + end-to-end tests
make clean
```

### 4.3 Encrypting

```bash
echo secret | bin/vigenere -e plain.txt > cipher.txt
```

The key is read from stdin; plaintext and key are reduced to lowercase letters.

### 4.4 Cracking

```bash
bin/vigenere crack cipher.txt run.idx [options]
```

```
--top N             print the best N afterwards (default 10)
--max-key-length N  try key lengths 1..N (default 12)
--max-candidates N  skip key lengths that would exceed N candidates in total
                    (default 150000000, about 4 GB of memory while building)
--model M           ranking model: unigram, bigram or trigram (default trigram; see section 3)
--threads N         worker threads (default: all)
```

Progress goes to stderr, the best candidates to stdout:

```
key length  1: 0 candidates (0.0s)
...
key length  8: 32461988 candidates (15.2s)
key length  9: skipped, more than 116754002 candidates (23.6s)
ranked 33245998 candidates by trigram (8.8s)
wrote run.idx: 33245998 candidates, 266.0 MB (47.6s in total)
note: key length 9 skipped: more than 116754002 candidates (too ambiguous to list)
         1  key secret        helloworld  [hello:8341][world:117]  score -34.03
         2  key zhtp          abuntingon  [a:5][bunting:20356][on:10]  score -35.30
```

Each line shows the rank, the key, the plaintext, its word split with each word's frequency rank, and the score.

### 4.5 Reading the index

```bash
bin/vigenere show run.idx --count                 # candidates per key length, notes
bin/vigenere show run.idx --first 20              # the 20 most probable
bin/vigenere show run.idx --nth 1000000           # one candidate by rank
bin/vigenere show run.idx --range 5000 5100       # a range of ranks
bin/vigenere show run.idx --rank-of helloworld    # where a given plaintext ranks
```

Lookups by rank are instant at any size. `--rank-of` scans the file (a few seconds for tens of millions of candidates). Scores are recomputed by the model the index is ranked by.

### 4.6 Re-ranking an index

```bash
bin/vigenere rerank run.idx run-bigram.idx --model bigram   # default model: trigram
```

Ranks the candidates of an existing index by another model and writes a new index (or overwrites the same file), without repeating the search: for 14 million candidates about 3 s with `bigram` and 4 s with `trigram` on 20 threads. Loading the trigram tables adds about 1.5 s to every command that uses them.

---

## 5. The Index File

A 4096-byte text header, then one 8-byte record per candidate, sorted by score (rank 1 first). Rank `r` is at byte `4096 + 8·(r − 1)`.

- **Header** (text lines): `VIGIDX 1`, the ciphertext, a fingerprint of the word list, the ranking model and a fingerprint of its tables, the maximum key length, the candidate count, the count per key length, and notes for skipped key lengths. An index can only be read with the word list it was built with and the model tables it was ranked with; `rerank` brings it up to date. Indexes written before models existed have no model line; they were ranked by the removed `zipf` model and must be updated with `rerank` before `show` reads them.
- **Record:** the key length in the top 6 bits and the key packed base 26 below. Plaintext, word split and score are recomputed from the key when read.

---

## 6. Performance

Measured on a 20-thread machine.

| Ciphertext | Key | Candidates | True plaintext (trigram) | Time |
|---|---|---|---|---|
| `iloveyou` (8 letters) | `lemon` | 4,692,041 | rank 3 | 2.7 s |
| `helloworld` (10 letters) | `secret` | 33,245,998 (key length 9 skipped) | rank 1 | 48 s |
| `iamtoldtogoandseeitbyfour` (25 letters, `bench/stress.sh`) | `qzxwvkjplmrt` | 55,577,417 | rank 9 | 3 min 17 s |

For the 25-letter stress case, key lengths 1–10 take a few seconds and key lengths 11 and 12 take the rest; the index is 445 MB. The search it replaced (a word-by-word constraint search) was extrapolated to take about two months for the same case.

Building needs about 28 bytes of memory per candidate, plus the DP columns (up to about 1 GB for 25 letters with key length 12).

---

## 7. Limitations

- **Dictionary coverage.** Every word of the plaintext must be in `txt/english_words.txt`. The list covers more than one might expect ("dont", "london", "email") but misses, for example, "usa", "covid", "smartphone", "wifi" and American "color".
- **Ambiguity.** When the text is short relative to the key, enormous numbers of word sequences fit, and no search can tell them apart: 25 letters with key length 12 give 48 million candidates; key length 9 on 10 letters gives hundreds of millions (skipped). Below about 1.5 × the key length (e.g. 13 letters with key length 10) a ciphertext has no unique reading, and such key lengths usually exceed `--max-candidates`. Only the ranking can choose.
- **Ranking.** Single-word models favour common words, so word salad like "may employ ushered triple tow" can outrank a real sentence: on the stress case the true plaintext is at rank 6,376 with `unigram`, 512 with `bigram` and 9 with `trigram` (4,980 with the removed `zipf`). What still ranks above it there is mostly the key prior: its 12-letter key costs about 10 nats more than the 9-letter key of the reading ranked first ([TODO.md](TODO.md)).
- **Skipped key lengths** (repeating ciphertext, too many candidates) are listed with `show --count`.

---

## 8. Tests and Benchmark

```bash
make test               # both suites below
bin/test_dp             # unit tests
bash tests/run_tests.sh # end-to-end tests
bash bench/stress.sh    # 25-letter, key-length-12 stress case → bench/stress.log
python3 bench/bench.py  # randomized grid → bench/results.csv
python3 bench/realistic.py  # real short phrases → bench/realistic-{bigram,trigram}.csv
```

- **Unit tests** (`tests/test_dp.cpp`): the search against an independent brute force for key lengths 1–5 on texts of 8, 12, 19 and 22 letters (2–5 rows, words crossing rows), and the index file: round trip, score order, one entry per plaintext, `rank_of`, skipped key lengths, rejection of non-index files; the language models (probabilities sum to 1, the split DPs against a brute force over all splits, the reported split has the reported score, word order matters only to `bigram`, a known triple beats its bigram estimate); `rerank` (same candidates in the new order, the model recorded and checked, re-ranking back in place restores the original file byte for byte, an index from before the models is rejected by the reader but re-ranked correctly).
- **End-to-end tests** (`tests/run_tests.sh`, about 30 minutes): 18 known plaintexts with key lengths 1–8 are encrypted with the tool, cracked, and must be found; the rank of each is printed (most are rank 1). Three plaintexts whose key is nearly as long as the text (13 letters with key length 10, 14 with 11, 15 with 12) must instead have that key length skipped as too ambiguous. Also command-line errors and repeating ciphertexts. Texts over 16 letters stop at key length 10 to keep the suite shorter; the stress script covers the full range. The suite caps its own memory at 10 GB.
- **Benchmark** (`bench/bench.py`): plaintexts from common words (8–24 letters) with random keys (2–12 letters); records the rank of the true plaintext, the candidate count and the time. These plaintexts are word salad drawn from single-word frequencies, so this benchmark measures the search, not the ranking.
- **Realistic benchmark** (`bench/realistic.py`): 105 real short phrases (`bench/phrases.txt`: messages, instructions, sayings, plain sentences; 9–23 letters), each with one key, alternately a common English word or random letters, of length 2 to 0.6 × the text length. Reports found / top-1 / top-10 / top-100 / top-1000, mean reciprocal rank and median rank, overall, by key type and by text length, and lists every plaintext ranked below 10 next to the reading ranked first. Indexes are kept in `bench/cache/`; each is re-ranked by every model in `--models` (default `bigram,trigram`), with results in `bench/realistic-MODEL.csv` and a side-by-side list of the cases where the models disagree, so models are compared on the same candidates without repeating the search.

---

## Data Sources

- Word list: [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt), in frequency order
- Word and word-pair counts: [count_1w.txt and count_2w.txt](https://norvig.com/ngrams/) by Peter Norvig, from the Google Web Trillion Word Corpus, reduced to the word list by `tools/make_ngram_tables.py`
- Word-triple counts: the first 300 MB (compressed; 33 million lines, 179 million words) of the English monolingual [OpenSubtitles v2018](https://opus.nlpl.eu/OpenSubtitles/corpus/version/OpenSubtitles) corpus from OPUS (P. Lison and J. Tiedemann, 2016, *OpenSubtitles2016: Extracting Large Parallel Corpora from Movie and TV Subtitles*), counted by `tools/count_trigrams.cpp`:
  ```bash
  curl -r 0-299999999 https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2018/mono/en.txt.gz | zcat \
    | ./count_trigrams txt/english_words.txt 15 txt/english_trigrams.txt txt/english_trigram_contexts.txt
  ```
  Subtitles were chosen over encyclopedic text because short ciphertexts are mostly short conversational messages. Tatoeba was left out on purpose: its everyday sentences overlap the realistic benchmark's phrases.
