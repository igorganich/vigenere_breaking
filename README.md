# Vigenère Cipher Breaker

A C++ tool that breaks **ultra-short** Vigenère ciphertexts (2–25 letters) without knowing the key, in English or German plaintext. It works in three stages:

1. **Search.** For every key length from 1 to 12 it finds **every** key whose plaintext splits completely into dictionary words. The search is exhaustive, so the true plaintext is always among the candidates, as long as its words are in the dictionary and its key length was not skipped.
2. **Rank.** It scores every candidate (tens to hundreds of millions) with a word-trigram language model and stores them, best first, in an index file you can query by rank: the best 10, the millionth, or any range.
3. **Rescore.** GPT-2, run inside the tool, reads the best 20,000 as whole sentences and reorders them by how much sense they make.

On 84 realistic short phrases (messages, instructions, sayings), the true plaintext comes first in 96% of cases (measured with the previous 44,274-word list). On the hardest test, 25 letters with a 12-letter key and 152 million candidates, the trigram model alone puts it 17th behind fluent-looking word salad; GPT-2 puts it first:

```
$ bin/vigenere crack cipher.txt run.idx --top 3
...
ranked 152345867 candidates by trigram (62.9s, sorted on disk in 3 runs)
wrote run.idx: 152345867 candidates, 1218.8 MB (1401.1s in total)
rescored the best 1000 with GPT-2 (9.4s, loading 0.9s): run.idx.lm
         1  key qzxwvkjplmrt  I am told to go and see it by four.           trigram #17       score -87.80
         2  key szlnvgumrzyf  Gay cops with old enemies ship.               trigram #92       score -96.56
         3  key mzllxgbubyn   May employ ushered triple tow.                trigram #14       score -99.69
```

---

## Table of Contents

1. [Background](#1-background)
2. [Stage 1: the Search](#2-stage-1-the-search)
3. [Stage 2: Ranking](#3-stage-2-ranking)
4. [Stage 3: Rescoring with GPT-2](#4-stage-3-rescoring-with-gpt-2)
5. [Building and Usage](#5-building-and-usage)
6. [The Index File](#6-the-index-file)
7. [Performance and Resources](#7-performance-and-resources)
8. [Limitations](#8-limitations)
9. [Tests and Benchmarks](#9-tests-and-benchmarks)
10. [Data Sources](#10-data-sources)

---

## 1. Background

### 1.1 The Vigenère cipher

Each plaintext letter is shifted by the corresponding letter of a repeating key. With key length `L` and letters as numbers `a=0 … z=25`:

```
c[i] = (p[i] + k[i mod L]) mod 26
p[i] = (c[i] - k[i mod L] + 26) mod 26
```

### 1.2 Why short ciphertexts are hard

Classic attacks (index of coincidence, frequency analysis, n-gram hill-climbing) need enough text per key letter to see English statistics. With 8–25 letters and a key of up to 12, each key letter decodes only one to three plaintext letters, so statistics say almost nothing. What remains is the constraint that the plaintext must be made of real words. This tool searches on that constraint directly and exhaustively, and uses language models only to order the readings it found.

### 1.3 Assumptions

- Ciphertext of **2–25 letters** (non-letters are stripped; longer input is rejected).
- Key length **1–12**.
- The plaintext is **English or German** (`--lang`, [§5.5](#55-languages)), written in the 26 letters a–z.
- The plaintext consists **entirely of dictionary words**: for English the bundled word list of 81,381 words plus "a" and "i", for German 66,222 words ([§10](#10-data-sources)), and any custom words given with `--words`, written without spaces or punctuation ("dont", not "don't"). A plaintext containing a word outside the list is not found (see [§8](#8-limitations)).

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
- **Compact columns:** a column's state is a tuple of row states, but each row takes far fewer distinct values in a column than there are tuples. So every column keeps, per row, a sorted table of its distinct states, and stores each state as one code: the row states' table positions packed into 64 bits (128 with 4–5 rows, key lengths 5–8). Each column is built in two passes over its successors, one for the tables and one for the codes. This takes 8 bytes per state instead of 8 per row, and is what lets the 81k-word list search key length 12 on 25 letters (about 150 million states in the largest column) within a few GB.
- **Words across row boundaries:** a row may start in the middle of a word. Its state remembers the tail it read; once all columns are done, the head left open at the end of the previous row plus that tail must form a dictionary word (a word may span several rows).
- **Backward pass:** states that cannot reach an accepted end are discarded.
- **Reading off keys:** the columns are walked again, carrying for each key prefix the *set* of states it can be in, so every key comes out exactly once. Keys are written to a temporary file as they are found, so memory does not grow with their number.
- **Short keys (1–4)** would split the text into up to 25 rows, but their key space is tiny (≤ 26⁴ = 456,976 keys), so they are searched by brute force.
- **Repeated keys:** a key like `abcabc` gives the same plaintext as `abc`; only the shortest form is kept, so every plaintext appears once.
- Work is spread over all CPU threads.

### 2.3 Key lengths that are skipped

`crack` skips a key length, with a note, in two cases:

- **The ciphertext repeats with period `L`.** Then the plaintext must repeat too, and any repeating English text is an equally good answer: the key length carries no information.
- **More candidates than `--max-candidates`** (default 1 billion, for all key lengths together). This happens when the key is nearly as long as the text: for key length 9 on 10 letters, only one plaintext letter is tied to another, so almost any word sequence fits and listing them is close to enumerating English. The limit bounds disk space and time (candidates are kept on disk, [§7](#7-performance-and-resources)), so it can be raised.

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

- **Triples** (`txt/en/trigrams.txt`): those seen at least 15 times in 179 million words of film subtitles (OpenSubtitles). `c(w₁ w₂)` (`txt/en/trigram_contexts.txt`) counts every continuation of the pair, including triples below the cutoff. `D = n₁ / (n₁ + 2n₂) ≈ 0.53` is the usual absolute discount, computed from the counts.
- **Back-off weight** `α(w₁ w₂)` gives the unlisted words exactly the probability the listed triples leave over, so `P(· | w₁ w₂)` sums to 1 over the dictionary.
- **Word pairs:** `P(w₂ | w₁) = c(w₁ w₂) / c(w₁)` for the 235,000 pairs in `txt/en/bigrams.txt` (Google Web Trillion Word Corpus, pairs seen at least 100,000 times), and `α(w₁) · P(w₂)` for every other pair, again normalized.
- **Single words:** `P(w) = c(w) / Σ c` (`txt/en/unigrams.txt`, same corpus).
- The first word of a text uses `P(w)`, the second `P(w₂ | w₁)`. A letter outside any word has log-probability −40, below any word, so it never wins; every candidate from the search splits fully into words anyway.

Nothing is tuned: every number comes from the counts.

The split is found by a second dynamic program (`src/cipher.hpp`), separate from the search. Its state is the last two words, i.e. a pair of adjacent dictionary words in the text; for 25 letters there are a few hundred such states, so scoring a candidate takes about a microsecond.

### 3.3 Other models

`--model bigram` (word pairs as above, with the last word as state) and `--model unigram` (single words) remain available for comparison. On the ranking benchmark ([§9](#9-tests-and-benchmarks), 84 phrases) and the stress case, both measured with the previous 44,274-word list (with the 81k-word list the stress case is 17th under trigram):

| Model | Ranked first | Mean reciprocal rank | Stress case (`bench/stress.sh`) |
|---|---|---|---|
| **trigram** (default) | **95%** | **0.956** | **rank 9** |
| bigram | 80% | 0.839 | rank 512 |
| unigram | not measured | not measured | rank 6,376 |

Of the 15 phrases where trigram and bigram disagree, trigram ranks 14 higher; for example "out of sight out of mind" is first under trigram and 40th under bigram. A first model with single-word probabilities taken from frequency rank (Zipf's law) ranked 9 of the first 46 phrases below first place, as far down as 136th, and the stress case 4,980th; it was removed.

---

## 4. Stage 3: Rescoring with GPT-2

### 4.1 Why

The trigram model sees three words at a time. "Rate me wow name medicine my or" is made of plausible pieces, and nothing in a three-word window says the whole is nonsense. Whether a sentence makes sense is a property of the whole sentence, so the best readings are read again by a neural language model that sees all of it.

### 4.2 How

The best 20,000 candidates of the trigram ranking (`--lm-top`) are turned into sentences from their word split ("I am told to go and see it by four.": spaces, a capital first letter, capital "I", a full stop) and ranked by

```
score = log P_GPT-2(sentence) − key_length · ln 26
```

the same posterior as before with a better estimate of P(plaintext), ties broken by trigram rank. The model is OpenAI's GPT-2 small (124 million parameters), and `log P` is the sum of the log-probabilities of the sentence's tokens, the first one after GPT-2's end-of-text token. The result is written next to the index as `<index>.lm`; the index keeps the trigram order, so every candidate stays reachable.

This is a cascade, not a filter: the search still lists every candidate and the trigram model still orders all of them, and GPT-2 only reorders the shortlist. On the ranking benchmark the worst trigram rank of a true plaintext is 11; with the 81k-word list some short texts fall further (in the end-to-end tests "quickfox" with key `cipher` is 6,001st), so the default shortlist is 20,000, about 3 minutes of GPT-2.

GPT-2 runs inside the tool (`src/gpt2.cpp`), with no dependencies: it reads the Hugging Face release (`model.safetensors`, `vocab.json`, `merges.txt`) directly and implements GPT-2's byte-level BPE tokenizer and its 12-layer transformer in C++, with an AVX2/FMA matrix kernel and a portable fallback. Its scores match the reference implementation (PyTorch, Hugging Face `transformers`) to within 0.0002 nats per sentence, and do not depend on the number of threads or on which sentences share a batch. `tools/get_gpt2.sh` downloads the model (about 550 MB, pinned to one revision and checked by SHA-256) into `models/gpt2/`; without it, `crack` says so and prints the trigram ranking.

### 4.3 Results

On the ranking benchmark (84 phrases; the same two are out of reach of the search for both), measured with the previous 44,274-word list; with the 81k-word list the stress case is 17th under trigram and still first after GPT-2:

| | Ranked first | Mean reciprocal rank | Stress case |
|---|---|---|---|
| trigram | 80 of 84 (95.2%) | 0.956 | rank 9 |
| **trigram, then GPT-2 on the best 1,000** | **81 of 84 (96.4%)** | **0.968** | **rank 1** |

GPT-2 moves "money talks" from 11th to first and "the coffee is too hot" from 4th to first. It moves one phrase down: "take the last train" (key `important`) is 3rd behind "Take the fast train." (key `importatt`), an equally good sentence that only the key can tell apart.

---

## 5. Building and Usage

### 5.1 Requirements

- GCC or Clang with C++17 support
- Python 3 (benchmarks and table generation only)
- About 550 MB of disk and 0.8 GB of memory for GPT-2 (optional; about 510 MB more for the German one)

### 5.2 Building

```bash
make          # optimized build → bin/vigenere
make test     # unit tests + end-to-end tests
make clean
bash tools/get_gpt2.sh   # GPT-2 for stage 3 → models/gpt2/ (optional, about 550 MB)
bash tools/get_gpt2.sh de   # German GPT-2 → models/gpt2-de/ (optional, about 510 MB)
```

The repository holds no binary: build it on the machine that runs it. The build uses `-march=native`, so the binary is tuned to that CPU and may not run on another one; rebuild there instead of copying it. The binary finds the word lists and model tables in `txt/<language>/` next to its own `bin/` directory, whatever the working directory.

### 5.3 Encrypting

```bash
echo secret | bin/vigenere -e plain.txt > cipher.txt
```

The key is read from stdin; plaintext and key are reduced to lowercase letters.

### 5.4 Cracking

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
--lm-top K          rescore the best K with GPT-2 (default 20000)
--no-lm             skip GPT-2 and print the trigram ranking
--lang L            plaintext language: en (default) or de (German); later
                    commands read it from the index
--words FILE        add the words of FILE to the dictionary (one or more per line,
                    '#' starts a comment); saved next to the index as <index>.words
--threads N         worker threads (default: all)
```

Custom words (`--words`) are for names and terms the list lacks. They are saved as `run.idx.words` and loaded by every later command on that index, which can only be read with the same words. The word-count tables have no counts for them, so each gets the table's smallest count: readings that contain them are ranked as if their custom words were very rare (see `TODO.md`). Every extra word, short ones especially, adds readings: a few hundred cryptography and modern words added 13% more candidates to a 25-letter ciphertext, most of them through 3-letter abbreviations.

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

(This run used `--max-candidates 150000000 --no-lm`; with the default, key length 9 is listed too, and the best candidates are then rescored with GPT-2 and printed as sentences, as in the example at the top.) Each line shows the rank, the key, the plaintext, its word split with each word's frequency rank in the word list, and the score.

### 5.5 Languages

```bash
bin/vigenere crack cipher.txt run.idx --lang de
```

`--lang de` cracks German plaintexts. German is written in the same 26 letters, with umlauts and ß spelled out as German ciphers write them: ä → ae, ö → oe, ü → ue, ß → ss ("schön" is `schoen`, Müller is `mueller`). Each language has its own data in `txt/<language>/` (word list, count tables) and its own GPT-2 in `models/`; the index records the language, so `show`, `rescore` and `rerank` need no option.

GPT-2 reads each word in its real spelling, kept in `txt/de/display.txt` from the most frequent spelling of the word in mid-sentence: the candidate `dashausistschoen` becomes "Das Haus ist schön." for the German GPT-2 ([dbmdz/german-gpt2](https://huggingface.co/dbmdz/german-gpt2), the same architecture as GPT-2 small; `tools/get_gpt2.sh de`).

The same ciphertext has far fewer German readings than English ones: German words are longer and its short words are few (24 of 2 letters, 142 of 3), so a 25-letter German text with an 8-letter key has thousands of candidates where an English reading of it has over a hundred million. A language is chosen up front; to try both, crack twice into two indexes.

Adding a language takes a word list, its count tables and, optionally, a display file and a GPT-2 of the same shape; `src/lang.hpp` lists the languages. The German data was built by `tools/count_words.cpp`, `tools/make_word_list_de.py` and `tools/count_trigrams.cpp` ([§10](#10-data-sources)).

### 5.6 Reading the index

```bash
bin/vigenere show run.idx --count                 # candidates per key length, notes
bin/vigenere show run.idx --first 20              # the 20 most probable
bin/vigenere show run.idx --nth 1000000           # one candidate by rank
bin/vigenere show run.idx --range 5000 5100       # a range of ranks
bin/vigenere show run.idx --rank-of helloworld    # where a given plaintext ranks (trigram)
bin/vigenere show run.idx --lm 20                 # the best 20 after GPT-2 (run.idx.lm)
```

Lookups by rank are instant at any size. `--rank-of` scans the file (a few seconds for tens of millions of candidates). Scores are recomputed with the model the index is ranked by.

### 5.7 Re-ranking an index

```bash
bin/vigenere rerank run.idx run-bigram.idx --model bigram   # default model: trigram
```

Ranks the candidates of an existing index by another model and writes a new index (or overwrites the same file), without repeating the search: for 14 million candidates about 3 s with `bigram` and 4 s with `trigram` on 20 threads. Loading the trigram tables adds about 1.5 s to every command that uses them. The GPT-2 shortlist of the old ranking is removed.

### 5.8 Rescoring with GPT-2

```bash
bin/vigenere rescore run.idx --top 5000 --print 20
```

Rescores the best `--top` (default 20000) of an existing index with GPT-2 into `run.idx.lm` and prints the best `--print` (default 10). `crack` does this itself unless `--no-lm` is given or the model is missing.

```bash
bin/vigenere rescore run.idx --ranks ranks.txt --scores scores.tsv --print 20
```

Rescores any chosen candidates instead: those at the trigram ranks listed in `ranks.txt` (one per line, any order), for example every candidate containing a certain word. Scores are appended to `scores.tsv` (`rank`, `key`, `score`, `sentence`, tab-separated) after every batch of 20,000, with progress and the time left on stderr; running the same command again skips the ranks already scored, so a long run can be interrupted and resumed. At the end the best `--print` of all scores in the file are printed. GPT-2 reads about 70–110 sentences per second on 20 threads, so a million candidates take 3–4 hours.

---

## 6. The Index File

A 4096-byte text header, then one 8-byte record per candidate, sorted by score (rank 1 first). Rank `r` is at byte `4096 + 8·(r − 1)`.

- **Header** (text lines): `VIGIDX 1`, the ciphertext, the language (`lang de`; indexes without the line are English), a fingerprint of the word list, the ranking model and a fingerprint of its tables, the maximum key length, the candidate count, the count per key length, and notes for skipped key lengths. An index can only be read with the word list it was built with and the model tables it was ranked with; `rerank` brings it up to date.
- **Record:** the key length in the top 6 bits and the key packed base 26 below. Plaintext, word split and score are recomputed from the key when read.
- **Older indexes** have no model line; they were ranked by the removed Zipf model. `show` rejects them; `rerank` converts them.
- **Custom words** (`<index>.words`, text): the words added with `--words`, one per line. Every command reads them with the index; without them the index is rejected as built with a different dictionary.
- **GPT-2 shortlist** (`<index>.lm`, text): a header naming the ciphertext, candidate count and ranking model of its index (`show --lm` refuses a shortlist that no longer matches), then one line per candidate, best first: trigram rank, key, score, sentence.

---

## 7. Performance and Resources

Measured on a 20-thread machine with 15 GB of memory. The first four rows were measured with the previous 44,274-word list; the larger list finds more candidates and takes longer, as the two stress-case rows show.

| Ciphertext | Key | Candidates | True plaintext | Time | Index |
|---|---|---|---|---|---|
| `iloveyou` (8 letters) | `lemon` | 4,692,041 | rank 3 | 2.7 s | 38 MB |
| `helloworld` (10 letters, `--max-candidates 150000000`) | `secret` | 33,245,998 (key length 9 skipped) | rank 1 | 48 s | 266 MB |
| `checktheseconddrawer` (20 letters) | `fczrzibvcca` | 280,914,433 (key length 12 skipped) | rank 1 | 7 min 8 s | 2.2 GB |
| `mybrotherlivesinlondon` (22 letters) | `increasingly` | 631,850,161 | rank 1 | 8 min 53 s | 5.1 GB |
| `iamtoldtogoandseeitbyfour` (25 letters, `bench/stress.sh`), 44k-word list | `qzxwvkjplmrt` | 55,577,417 | rank 9 (GPT-2: 1) | 2 min 58 s | 445 MB |
| the same, 81k-word list | `qzxwvkjplmrt` | 152,345,867 | rank 17 (GPT-2: 1) | 23 min 21 s | 1.2 GB |

True-plaintext ranks are the trigram's; times are for the search and the trigram ranking. GPT-2 adds 1 s to load and 5–10 s per 1,000 sentences, so about 3 minutes for the default 20,000 (0.8 GB of memory). Key lengths 11 and 12 take almost all the time on long texts; shorter key lengths take seconds. The word-by-word constraint search this tool started with was extrapolated to take about two months for the stress case.

**Memory and disk.** Candidates never sit in memory. The search writes them to a temporary file next to the index, and ranking sorts them on disk: chunks of 64 million candidates (about 1 GB) are scored and sorted, written as sorted runs, and merged into the index. Input that fits in one chunk is written directly; the result is the same file either way.

- **Memory:** a few GB at most, whatever the number of candidates: the search's DP columns plus one sort chunk. With the 81k-word list, 25 letters with key length 12 peak at about 3.2 GB. The 20- and 22-letter texts above needed more than 15 GB before candidates moved to disk.
- **Disk:** about 24 bytes per candidate while building (keys plus sorted runs), 8 in the finished index. Put the index on a real disk: on a RAM disk such as a tmpfs `/tmp`, temporary files use memory.
- Temporary files are removed when the build finishes or fails; running out of memory or disk is reported as an error.

---

## 8. Limitations

- **Dictionary coverage.** Every word of the plaintext must be in the language's word list (`txt/en/words.txt`, `txt/de/words.txt`) or a `--words` file. The German list holds words seen at least 20 times in German subtitles, so rare compounds are missing. The list covers more than one might expect ("dont", "london", "email", "smartphone", "wifi") but misses, for example, "usa" and "covid", and of 2–3-letter words it has only the 631 of the original list, on purpose: the short words other lists add are mostly abbreviations ("pm", "cd", "ltd"), and each multiplies the readings of every ciphertext.
- **Ambiguity.** When the text is short relative to the key, enormous numbers of word sequences fit, and no search can tell them apart: 25 letters with key length 12 give 48 million candidates, 22 letters with key length 12 nearly 600 million. Below about 1.5 × the key length (e.g. 13 letters with key length 10) a ciphertext has no unique reading, and such key lengths can exceed even the default limit of a billion candidates. Only the ranking can choose.
- **Ranking.** With GPT-2, the remaining mistakes are between readings that are all good English: "take the last train" (key `important`) is 3rd behind "Take the fast train." (key `importatt`). Only the key could tell them apart, and the tool treats every key as random letters. Without GPT-2, the trigram model fails mainly on texts that are short for their key: "money talks" (10 letters, key length 6) is 11th behind "as that mean", and the stress case 17th (9th with the 44k-word list, behind "a jury us coupe frog the autumn").
- **GPT-2's shortlist** holds the trigram's best 20,000. A true plaintext the trigram model ranks lower is not rescored; `rescore --top` widens the shortlist (about 5–10 s per 1,000), and `rescore --ranks` rescores any chosen candidates.
- **Skipped key lengths** (repeating ciphertext, too many candidates) are listed with `show --count`.

---

## 9. Tests and Benchmarks

```bash
make test                    # both test suites
bin/test_dp                  # unit tests
bash tests/run_tests.sh      # end-to-end tests
bash bench/stress.sh         # 25-letter, key-length-12 stress case → bench/stress.log
python3 bench/bench.py       # randomized grid → bench/results.csv
python3 bench/realistic.py   # real short phrases → bench/realistic-{trigram,gpt2}.csv
```

- **Unit tests** (`tests/test_dp.cpp`, `tests/test_gpt2.cpp`):
  - the search against an independent brute force for key lengths 1–5 on texts of 8, 12, 19 and 22 letters (2–5 rows, words crossing rows);
  - the language models: probabilities sum to 1, the split DPs against a brute force over all splits, the reported split has the reported score, word order matters to `bigram` but not `unigram`, a known triple beats its bigram estimate;
  - the index file: round trip, score order, one entry per plaintext, `rank_of`, skipped key lengths, rejection of non-index files;
  - `rerank`: same candidates in the new order, the model recorded and checked, re-ranking back in place restores the original file byte for byte, an older index is rejected by the reader but converted;
  - building on disk: tiny sort chunks force sorted runs and a merge and must give the same file byte for byte, a key length over the limit leaves nothing behind, no temporary files remain;
  - the GPT-2 stage: the sentences it reads, the shortlist file (round trip, rejected once its index is re-ranked);
  - languages: 1-letter words per language, German spellings in GPT-2's sentences (schön, a capital Ü), a German index that finds its plaintext, records its language and is rejected with the English dictionary;
  - GPT-2 itself (skipped without `models/gpt2`): token ids and log-probabilities of seven sentences against the reference implementation, the same values bit for bit with 1 or 20 threads, alone or in a batch, and over several batches.
- **End-to-end tests** (`tests/run_tests.sh`, about 30 minutes): 18 known plaintexts with key lengths 1–9 are encrypted with the tool, cracked, and must be found; the rank of each is printed (most are rank 1). Three plaintexts whose key is nearly as long as the text (13 letters with key length 10, 14 with 11, 15 with 12) must instead have that key length skipped as too ambiguous. Also command-line errors and repeating ciphertexts, and (with `models/gpt2`) the GPT-2 stage: "money talks" with key `merely`, 11th by trigram, must come first, rescoring chosen ranks must resume without repeating any, and re-ranking must drop the stale shortlist. Custom words: a plaintext with a made-up word is found only with `--words`, the word list is saved next to the index, and the index is rejected without it. German: `dashausistschoen` with key `abc` is found first with `--lang de`, the index records the language, and (with `models/gpt2-de`) the German GPT-2 reads "Das Haus ist schön." first. The suite uses `--max-candidates 150000000 --no-lm` except in the GPT-2 tests, stops texts over 16 letters at key length 10 to stay short (the stress script covers the full range), and caps its own memory at 10 GB.
- **Search benchmark** (`bench/bench.py`): plaintexts from common words (8–24 letters) with random keys (2–12 letters); records the rank of the true plaintext, the candidate count and the time. These plaintexts are word salad, so this benchmark measures the search, not the ranking.
- **Ranking benchmark** (`bench/realistic.py`): 105 real short phrases (`bench/phrases.txt`: messages, instructions, sayings, plain sentences; 9–23 letters), each with one key, alternately a common English word or random letters, of length 2 to 0.6 × the text length. Reports found / top-1 / top-10 / top-100 / top-1000, mean reciprocal rank and median rank, overall, by key type and by text length, and lists every plaintext ranked below 10 next to the reading ranked first. Each search is cached in `bench/cache/` and re-ranked by every model in `--models` (`unigram`, `bigram`, `trigram`, and `gpt2` for GPT-2 on the trigram's best `--lm-top`, 1,000 in the benchmark; default `trigram,gpt2`), so models are compared on the same candidates; `--summary` adds a side-by-side list of the phrases where they disagree, and `--cached-only` measures a new model without searching again.

---

## 10. Data Sources

### English

- **Word list** (`txt/en/words.txt`): 81,381 words in order of frequency, built by `tools/make_word_list.py` from
  - the 44,274 words of the previous list, taken from the corncob list (`corncob_lowercase.txt`, 58,109 words, by mieliestronk.com; the original link is gone, an [archived copy](https://web.archive.org/web/20191214032117/http://www.mieliestronk.com/corncob_lowercase.txt) remains). Its 631 words of 2–3 letters are the only short words;
  - [SCOWL](http://wordlist.aspell.net/) (Kevin Atkinson), as Debian's `wamerican-large` (size 70) and `wamerican-huge` (size 80), version 2020.12.07: words of 4+ letters of size 70 that occur in Norvig's `count_1w.txt` (about 28,600), names seen at least 100,000 times there (7,511), and words of size 80 seen at least 100,000 times that are a prefix or suffix applied to a listed word (963);
  - "vigenere", which neither source has.

  ```bash
  git show 719342e:txt/english_words.txt > base.txt
  apt-get download wamerican-large wamerican-huge   # then dpkg-deb -x each; copy
                                                    # usr/share/dict/american-english-{large,huge}
  python3 tools/make_word_list.py base.txt DIR      # DIR: the two lists and count_1w.txt
  ```
  The word-count tables below must be rebuilt after the list changes.
- **Word and word-pair counts:** [count_1w.txt and count_2w.txt](https://norvig.com/ngrams/) by Peter Norvig, from the Google Web Trillion Word Corpus, reduced to the word list by `tools/make_ngram_tables.py`:
  ```bash
  python3 tools/make_ngram_tables.py DIR_WITH_count_1w.txt_AND_count_2w.txt
  ```
- **Word-triple counts:** the first 300 MB (compressed; 33 million lines, 179 million words) of the English monolingual [OpenSubtitles v2018](https://opus.nlpl.eu/OpenSubtitles/corpus/version/OpenSubtitles) corpus from OPUS (P. Lison and J. Tiedemann, 2016, *OpenSubtitles2016: Extracting Large Parallel Corpora from Movie and TV Subtitles*), counted by `tools/count_trigrams.cpp`:
  ```bash
  g++ -O2 -std=c++17 -o count_trigrams tools/count_trigrams.cpp
  curl -r 0-299999999 https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2018/mono/en.txt.gz | zcat \
    | ./count_trigrams txt/en/words.txt 15 txt/en/trigrams.txt txt/en/trigram_contexts.txt
  ```
  Subtitles were chosen over encyclopedic text because short ciphertexts are mostly short conversational messages. Tatoeba was left out on purpose: its everyday sentences overlap the ranking benchmark's phrases.
- **GPT-2 small:** [openai-community/gpt2](https://huggingface.co/openai-community/gpt2) on Hugging Face (OpenAI; A. Radford et al., 2019, *Language Models are Unsupervised Multitask Learners*; MIT licence), revision `607a30d`, downloaded by `tools/get_gpt2.sh`.

### German

- **Corpus:** the whole German monolingual [OpenSubtitles v2018](https://opus.nlpl.eu/OpenSubtitles/corpus/version/OpenSubtitles) corpus from OPUS (476 MB compressed; 41.6 million lines, 219 million words). Every count below comes from it, umlauts and ß transliterated (ae, oe, ue, ss).
- **Word list** (`txt/de/words.txt`, `unigrams.txt`, `display.txt`): 66,222 words that occur in the corpus and in the German spelling list of Debian's `wngerman` package (igerman98 by Björn Jacke, version 20161207, GPL-2+; the word list is a selection from it): words of 4+ letters seen at least 20 times, 142 words of 3 letters seen at least 2,000 times except names and English words, 24 hand-picked words of 2 letters, and "vigenere". Built by `tools/count_words.cpp` (word counts and the usual spelling of each word) and `tools/make_word_list_de.py`:
  ```bash
  g++ -O2 -std=c++17 -o count_words tools/count_words.cpp
  curl https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2018/mono/de.txt.gz -o de.txt.gz
  zcat de.txt.gz | ./count_words 5 > de_counts.txt
  apt-get download wngerman                          # then dpkg-deb -x; usr/share/dict/ngerman
  python3 tools/make_word_list_de.py de_counts.txt ngerman
  ```
- **Word pairs and triples** (`txt/de/bigrams.txt`, `trigrams.txt`, `trigram_contexts.txt`): those seen at least 15 times in the same corpus (632,345 pairs, 832,946 triples), counted by `tools/count_trigrams.cpp`:
  ```bash
  zcat de.txt.gz | ./count_trigrams txt/de/words.txt 15 txt/de/trigrams.txt txt/de/trigram_contexts.txt txt/de/bigrams.txt
  ```
- **German GPT-2:** [dbmdz/german-gpt2](https://huggingface.co/dbmdz/german-gpt2) (Bavarian State Library MDZ Digital Library team; MIT licence), revision `0579faa`, downloaded by `tools/get_gpt2.sh de`. Its scores match the reference implementation to within 0.0002 nats per sentence, as for the English model.
