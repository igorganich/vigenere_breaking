# Vigenère Cipher Breaker

A C++ tool that breaks **ultra-short** Vigenère ciphertexts (2–25 letters) without knowing the key. It works in two stages:

1. **Search.** For every key length from 1 to 12 it finds **every** key whose plaintext splits completely into dictionary words. The search is exhaustive, so the true plaintext is always among the candidates, as long as its words are in the dictionary and its key length was not skipped.
2. **Rank.** It scores every candidate with a word-trigram language model and stores them, best first, in an index file you can query by rank: the best 10, the millionth, or any range.

On 84 realistic short phrases (messages, instructions, sayings), the true plaintext is ranked first in 95% of cases.

```
$ bin/vigenere crack cipher.txt run.idx --top 3
...
ranked 631850161 candidates by trigram (131.5s, sorted on disk in 10 runs)
wrote run.idx: 631850161 candidates, 5054.8 MB (533.2s in total)
         1  key increasingly  mybrotherlivesinlondon  [my:81][brother:787][lives:724][in:5][london:748]  score -61.95
         2  key bxfeapmmdgp   toyesenablespageactive  [to:4][yes:1123][enables:6534][page:1139][active:1151]  score -73.29
         3  key increasinglg  mybrotherlinesinlondon  [my:81][brother:787][lines:1008][in:5][london:748]  score -74.02
```

---

## Table of Contents

1. [Background](#1-background)
2. [Stage 1: the Search](#2-stage-1-the-search)
3. [Stage 2: Ranking](#3-stage-2-ranking)
4. [Building and Usage](#4-building-and-usage)
5. [The Index File](#5-the-index-file)
6. [Performance and Resources](#6-performance-and-resources)
7. [Limitations](#7-limitations)
8. [Tests and Benchmarks](#8-tests-and-benchmarks)
9. [Data Sources](#9-data-sources)

---

## 1. Background

### 1.1 The Vigenère cipher

Each plaintext letter is shifted by the corresponding letter of a repeating key. With key length `L` and letters as numbers `a=0 … z=25`:

```
c[i] = (p[i] + k[i mod L]) mod 26
p[i] = (c[i] - k[i mod L] + 26) mod 26
```

### 1.2 Why short ciphertexts are hard

Classic attacks (index of coincidence, frequency analysis, n-gram hill-climbing) need enough text per key letter to see English statistics. With 8–25 letters and a key of up to 12, each key letter decodes only one to three plaintext letters, so statistics say almost nothing. What remains is the constraint that the plaintext must be made of real words. This tool searches on that constraint directly and exhaustively, and uses statistics only to order the readings it found.

### 1.3 Assumptions

- Ciphertext of **2–25 letters** (non-letters are stripped; longer input is rejected).
- Key length **1–12**.
- The plaintext consists **entirely of dictionary words**: the bundled word list of 44,275 words plus "a" and "i", written without spaces or punctuation ("dont", not "don't"). A plaintext containing a word outside the list is not found (see [§7](#7-limitations)).

---

## 2. Stage 1: the Search

### 2.1 In simple words

Write the plaintext in rows of `L` letters, one row per repetition of the key:

```
row 1:  i a m t o l d t o g o a     ← positions 1–12
row 2:  n d s e e i t b y f o u     ← positions 13–24
row 3:  r                           ← position 25
```

Each **column** belongs to one key letter, and choosing that key letter fills in the whole column at once. So the search goes column by column, trying all 26 key letters, and in each row keeps reading as if spelling words: is what this row has read so far still the start of some word, or a finished word? If any row stops making sense, that choice dies.

After a few columns, all that matters for the rest of the search is a small **bookmark** per row: "row 1 is three letters into a word starting with *to…*", "row 2 just finished a word". *Which* key letters led there no longer matters: those columns are done and never read again. Huge numbers of different key prefixes lead to the same bookmarks, and the search merges them and continues from each bookmark once. That is what makes an exhaustive search feasible: for 25 letters and key length 12 the number of distinct bookmark combinations peaks around 30 million, against roughly 10¹⁷ possible keys.

The search knows only which strings are words, not how likely they are. Probabilities come in at stage 2, so every model ranks the same complete set of candidates, and a better model can be applied later to an existing index without searching again.

### 2.2 Details

This is a dynamic program over key columns (`src/dp.cpp`):

- **State per row (track):** either *inside a word that started in this row*, as a node of the dictionary trie (the root means "at a word boundary"); or *reading the tail of a word that started in an earlier row*, as a node of a trie of all proper word suffixes. Each column's states are deduplicated.
- **Words across row boundaries:** a row may start in the middle of a word. Its state remembers the tail it read; once all columns are done, the head left open at the end of the previous row plus that tail must form a dictionary word (a word may span several rows).
- **Backward pass:** states that cannot reach an accepted end are discarded.
- **Reading off keys:** the columns are walked again, carrying for each key prefix the *set* of states it can be in, so every key comes out exactly once. Keys are written to a temporary file as they are found, so memory does not grow with their number.
- **Short keys (1–4)** would split the text into up to 25 rows, but their key space is tiny (≤ 26⁴ = 456,976 keys), so they are searched by brute force.
- **Repeated keys:** a key like `abcabc` gives the same plaintext as `abc`; only the shortest form is kept, so every plaintext appears once.
- Work is spread over all CPU threads.

### 2.3 Key lengths that are skipped

`crack` skips a key length, with a note, in two cases:

- **The ciphertext repeats with period `L`.** Then the plaintext must repeat too, and any repeating English text is an equally good answer: the key length carries no information.
- **More candidates than `--max-candidates`** (default 1 billion, for all key lengths together). This happens when the key is nearly as long as the text: for key length 9 on 10 letters, only one plaintext letter is tied to another, so almost any word sequence fits and listing them is close to enumerating English. The limit bounds disk space and time (candidates are kept on disk, [§6](#6-performance-and-resources)), so it can be raised.

---

## 3. Stage 2: Ranking

### 3.1 The score

Each candidate gets its posterior log-probability:

```
score = log P(plaintext) − key_length · ln 26
```

- **log P(plaintext)** is the probability of the most probable split of the plaintext into words, under a word-trigram model.
- **− key_length · ln 26** is the prior of a random key: there are 26^L keys of length L, so a fluent plaintext from a short key is stronger evidence than the same from a long one.

Ties are broken by key, so the order is deterministic.

### 3.2 The trigram model

The probability of each word given the two before it, with back-off when a triple is unknown:

```
P(w₃ | w₁ w₂) = (c(w₁ w₂ w₃) − D) / c(w₁ w₂)     for the 978,000 triples in the table
              = α(w₁ w₂) · P(w₃ | w₂)             for every other word
```

- **Triples** (`txt/english_trigrams.txt`): those seen at least 15 times in 179 million words of film subtitles (OpenSubtitles). `c(w₁ w₂)` (`txt/english_trigram_contexts.txt`) counts every continuation of the pair, including triples below the cutoff. `D = n₁ / (n₁ + 2n₂) ≈ 0.53` is the usual absolute discount, computed from the counts.
- **Back-off weight** `α(w₁ w₂)` gives the unlisted words exactly the probability the listed triples leave over, so `P(· | w₁ w₂)` sums to 1 over the dictionary.
- **Word pairs:** `P(w₂ | w₁) = c(w₁ w₂) / c(w₁)` for the 235,000 pairs in `txt/english_bigrams.txt` (Google Web Trillion Word Corpus, pairs seen at least 100,000 times), and `α(w₁) · P(w₂)` for every other pair, again normalized.
- **Single words:** `P(w) = c(w) / Σ c` (`txt/english_unigrams.txt`, same corpus).
- The first word of a text uses `P(w)`, the second `P(w₂ | w₁)`. A letter outside any word has log-probability −40, below any word, so it never wins; every candidate from the search splits fully into words anyway.

Nothing is tuned: every number comes from the counts.

The split is found by a second dynamic program (`src/cipher.hpp`), separate from the search. Its state is the last two words, i.e. a pair of adjacent dictionary words in the text; for 25 letters there are a few hundred such states, so scoring a candidate takes about a microsecond.

### 3.3 Other models

`--model bigram` (word pairs as above, with the last word as state) and `--model unigram` (single words) remain available for comparison. On the ranking benchmark ([§8](#8-tests-and-benchmarks), 84 phrases) and the stress case:

| Model | Ranked first | Mean reciprocal rank | Stress case (`bench/stress.sh`) |
|---|---|---|---|
| **trigram** (default) | **95%** | **0.956** | **rank 9** |
| bigram | 80% | 0.839 | rank 512 |
| unigram | not measured | not measured | rank 6,376 |

Of the 15 phrases where trigram and bigram disagree, trigram ranks 14 higher; for example "out of sight out of mind" is first under trigram and 40th under bigram. A first model with single-word probabilities taken from frequency rank (Zipf's law) ranked 9 of the first 46 phrases below first place, as far down as 136th, and the stress case 4,980th; it was removed.

---

## 4. Building and Usage

### 4.1 Requirements

- GCC or Clang with C++17 support
- Python 3 (benchmarks and table generation only)

### 4.2 Building

```bash
make          # optimized build → bin/vigenere
make test     # unit tests + end-to-end tests
make clean
```

The repository holds no binary: build it on the machine that runs it. The build uses `-march=native`, so the binary is tuned to that CPU and may not run on another one; rebuild there instead of copying it. The binary finds the word list and model tables in `txt/` next to its own `bin/` directory, whatever the working directory.

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
                    (default 1000000000; candidates are kept on disk next to the
                    index: about 24 bytes each while building, 8 in the index)
--model M           ranking model: trigram (default), bigram or unigram
--threads N         worker threads (default: all)
```

Progress goes to stderr, the best candidates to stdout:

```
key length  1: 0 candidates (0.0s)
...
key length  8: 32461988 candidates (15.3s)
key length  9: skipped, more than 116754002 candidates (22.4s)
ranked 33245998 candidates by trigram (8.8s)
wrote run.idx: 33245998 candidates, 266.0 MB (47.6s in total)
note: key length 9 skipped: more than 116754002 candidates (too ambiguous to list)
         1  key secret        helloworld  [hello:8341][world:117]  score -34.03
         2  key zhtp          abuntingon  [a:5][bunting:20356][on:10]  score -35.30
```

(This run used `--max-candidates 150000000`; with the default, key length 9 is listed too.) Each line shows the rank, the key, the plaintext, its word split with each word's frequency rank in the word list, and the score.

### 4.5 Reading the index

```bash
bin/vigenere show run.idx --count                 # candidates per key length, notes
bin/vigenere show run.idx --first 20              # the 20 most probable
bin/vigenere show run.idx --nth 1000000           # one candidate by rank
bin/vigenere show run.idx --range 5000 5100       # a range of ranks
bin/vigenere show run.idx --rank-of helloworld    # where a given plaintext ranks
```

Lookups by rank are instant at any size. `--rank-of` scans the file (a few seconds for tens of millions of candidates). Scores are recomputed with the model the index is ranked by.

### 4.6 Re-ranking an index

```bash
bin/vigenere rerank run.idx run-bigram.idx --model bigram   # default model: trigram
```

Ranks the candidates of an existing index by another model and writes a new index (or overwrites the same file), without repeating the search: for 14 million candidates about 3 s with `bigram` and 4 s with `trigram` on 20 threads. Loading the trigram tables adds about 1.5 s to every command that uses them.

---

## 5. The Index File

A 4096-byte text header, then one 8-byte record per candidate, sorted by score (rank 1 first). Rank `r` is at byte `4096 + 8·(r − 1)`.

- **Header** (text lines): `VIGIDX 1`, the ciphertext, a fingerprint of the word list, the ranking model and a fingerprint of its tables, the maximum key length, the candidate count, the count per key length, and notes for skipped key lengths. An index can only be read with the word list it was built with and the model tables it was ranked with; `rerank` brings it up to date.
- **Record:** the key length in the top 6 bits and the key packed base 26 below. Plaintext, word split and score are recomputed from the key when read.
- **Older indexes** have no model line; they were ranked by the removed Zipf model. `show` rejects them; `rerank` converts them.

---

## 6. Performance and Resources

Measured on a 20-thread machine with 15 GB of memory.

| Ciphertext | Key | Candidates | True plaintext | Time | Index |
|---|---|---|---|---|---|
| `iloveyou` (8 letters) | `lemon` | 4,692,041 | rank 3 | 2.7 s | 38 MB |
| `helloworld` (10 letters, `--max-candidates 150000000`) | `secret` | 33,245,998 (key length 9 skipped) | rank 1 | 48 s | 266 MB |
| `checktheseconddrawer` (20 letters) | `fczrzibvcca` | 280,914,433 (key length 12 skipped) | rank 1 | 7 min 8 s | 2.2 GB |
| `mybrotherlivesinlondon` (22 letters) | `increasingly` | 631,850,161 | rank 1 | 8 min 53 s | 5.1 GB |
| `iamtoldtogoandseeitbyfour` (25 letters, `bench/stress.sh`) | `qzxwvkjplmrt` | 55,577,417 | rank 9 | 2 min 58 s | 445 MB |

Key lengths 11 and 12 take almost all the time on long texts; shorter key lengths take seconds. The word-by-word constraint search this tool started with was extrapolated to take about two months for the stress case.

**Memory and disk.** Candidates never sit in memory. The search writes them to a temporary file next to the index, and ranking sorts them on disk: chunks of 64 million candidates (about 1 GB) are scored and sorted, written as sorted runs, and merged into the index. Input that fits in one chunk is written directly; the result is the same file either way.

- **Memory:** about 2.3 GB at most, whatever the number of candidates: the search's DP columns plus one sort chunk. The 20- and 22-letter texts above needed more than 15 GB before candidates moved to disk.
- **Disk:** about 24 bytes per candidate while building (keys plus sorted runs), 8 in the finished index. Put the index on a real disk: on a RAM disk such as a tmpfs `/tmp`, temporary files use memory.
- Temporary files are removed when the build finishes or fails; running out of memory or disk is reported as an error.

---

## 7. Limitations

- **Dictionary coverage.** Every word of the plaintext must be in `txt/english_words.txt`. The list covers more than one might expect ("dont", "london", "email") but misses, for example, "usa", "covid", "smartphone", "wifi" and American "color".
- **Ambiguity.** When the text is short relative to the key, enormous numbers of word sequences fit, and no search can tell them apart: 25 letters with key length 12 give 48 million candidates, 22 letters with key length 12 nearly 600 million. Below about 1.5 × the key length (e.g. 13 letters with key length 10) a ciphertext has no unique reading, and such key lengths can exceed even the default limit of a billion candidates. Only the ranking can choose.
- **Ranking.** The trigram model fails mainly on texts that are short for their key: "money talks" (10 letters, key length 6) is 11th behind "as that mean", "the coffee is too hot" (15 letters, key length 10) 4th behind "the alien customers", and on the stress case "a jury us coupe frog the autumn" is first. There the key prior decides much of the order: the stress plaintext's 12-letter key costs about 10 nats more than the 9-letter key of the reading ranked first. Ideas are in [TODO.md](TODO.md).
- **Skipped key lengths** (repeating ciphertext, too many candidates) are listed with `show --count`.

---

## 8. Tests and Benchmarks

```bash
make test                    # both test suites
bin/test_dp                  # unit tests
bash tests/run_tests.sh      # end-to-end tests
bash bench/stress.sh         # 25-letter, key-length-12 stress case → bench/stress.log
python3 bench/bench.py       # randomized grid → bench/results.csv
python3 bench/realistic.py   # real short phrases → bench/realistic-{bigram,trigram}.csv
```

- **Unit tests** (`tests/test_dp.cpp`):
  - the search against an independent brute force for key lengths 1–5 on texts of 8, 12, 19 and 22 letters (2–5 rows, words crossing rows);
  - the language models: probabilities sum to 1, the split DPs against a brute force over all splits, the reported split has the reported score, word order matters to `bigram` but not `unigram`, a known triple beats its bigram estimate;
  - the index file: round trip, score order, one entry per plaintext, `rank_of`, skipped key lengths, rejection of non-index files;
  - `rerank`: same candidates in the new order, the model recorded and checked, re-ranking back in place restores the original file byte for byte, an older index is rejected by the reader but converted;
  - building on disk: tiny sort chunks force sorted runs and a merge and must give the same file byte for byte, a key length over the limit leaves nothing behind, no temporary files remain.
- **End-to-end tests** (`tests/run_tests.sh`, about 30 minutes): 18 known plaintexts with key lengths 1–9 are encrypted with the tool, cracked, and must be found; the rank of each is printed (most are rank 1). Three plaintexts whose key is nearly as long as the text (13 letters with key length 10, 14 with 11, 15 with 12) must instead have that key length skipped as too ambiguous. Also command-line errors and repeating ciphertexts. The suite uses `--max-candidates 150000000`, stops texts over 16 letters at key length 10 to stay short (the stress script covers the full range), and caps its own memory at 10 GB.
- **Search benchmark** (`bench/bench.py`): plaintexts from common words (8–24 letters) with random keys (2–12 letters); records the rank of the true plaintext, the candidate count and the time. These plaintexts are word salad, so this benchmark measures the search, not the ranking.
- **Ranking benchmark** (`bench/realistic.py`): 105 real short phrases (`bench/phrases.txt`: messages, instructions, sayings, plain sentences; 9–23 letters), each with one key, alternately a common English word or random letters, of length 2 to 0.6 × the text length. Reports found / top-1 / top-10 / top-100 / top-1000, mean reciprocal rank and median rank, overall, by key type and by text length, and lists every plaintext ranked below 10 next to the reading ranked first. Each search is cached in `bench/cache/` and re-ranked by every model in `--models` (default `bigram,trigram`), so models are compared on the same candidates; `--summary` adds a side-by-side list of the phrases where they disagree, and `--cached-only` measures a new model without searching again.

---

## 9. Data Sources

- **Word list:** [corncob_lowercase.txt](http://www.mieliestronk.com/corncob_lowercase.txt), in frequency order.
- **Word and word-pair counts:** [count_1w.txt and count_2w.txt](https://norvig.com/ngrams/) by Peter Norvig, from the Google Web Trillion Word Corpus, reduced to the word list by `tools/make_ngram_tables.py`:
  ```bash
  python3 tools/make_ngram_tables.py DIR_WITH_count_1w.txt_AND_count_2w.txt
  ```
- **Word-triple counts:** the first 300 MB (compressed; 33 million lines, 179 million words) of the English monolingual [OpenSubtitles v2018](https://opus.nlpl.eu/OpenSubtitles/corpus/version/OpenSubtitles) corpus from OPUS (P. Lison and J. Tiedemann, 2016, *OpenSubtitles2016: Extracting Large Parallel Corpora from Movie and TV Subtitles*), counted by `tools/count_trigrams.cpp`:
  ```bash
  g++ -O2 -std=c++17 -o count_trigrams tools/count_trigrams.cpp
  curl -r 0-299999999 https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2018/mono/en.txt.gz | zcat \
    | ./count_trigrams txt/english_words.txt 15 txt/english_trigrams.txt txt/english_trigram_contexts.txt
  ```
  Subtitles were chosen over encyclopedic text because short ciphertexts are mostly short conversational messages. Tatoeba was left out on purpose: its everyday sentences overlap the ranking benchmark's phrases.
