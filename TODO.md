# TODO

## Ranking

With GPT-2 rescoring the trigram's best 1,000 (the default is now 20,000), the true plaintext comes first for 96% of the
phrases in `bench/realistic.py` (81 of 84; the other two are out of the search's reach, and
"take the last train" is 3rd behind the equally good "take the fast train"). Ideas; each
can be measured on the cached searches with `python3 bench/realistic.py --cached-only`:

- **Harder benchmark cases.** Almost every phrase in `bench/phrases.txt` is now ranked
  first, so the benchmark no longer separates models. Cases with keys close to the text
  length would, and would show whether a shortlist of 1,000 is always wide enough (the
  worst trigram rank of a true plaintext so far is 11).
- **A larger neural model.** GPT-2 small (124M parameters) is the cheapest that works. A
  stronger one (GPT-2 medium, or a recent small model) may separate readings that GPT-2
  small finds equally good, at a few times the cost per sentence; the loader and the
  tokenizer are GPT-2 specific.
- **Faster GPT-2.** About 5 s per 1,000 sentences on 20 threads. A persistent thread pool
  (threads are started per matrix product now) and smaller weights (bf16 or 8-bit) would
  cut that, and allow wider shortlists.
- **Larger trigram table.** 978k triples (count ≥ 15, 18 MB) from 179M words. It now only
  decides what reaches GPT-2; more corpus and a lower cutoff would raise the trigram rank
  of real phrases further.

## Probabilities for custom words

`crack --words FILE` adds words to the dictionary, but the tables have no counts for them,
so each gets the smallest count in the unigram table, as if it were as rare as the rarest
word. Readings with a custom word are then ranked low by the trigram model and seldom reach
GPT-2's shortlist. In one test on a 25-letter ciphertext, about 200 cryptography and
modern words added 15.4M candidates (117.0M → 132.5M), mostly through 3-letter
abbreviations, and the trigram top 10 did not change. Rescoring all of them with GPT-2
would take about 37 hours (114 sentences per second); `rescore --ranks` can rescore a
chosen subset. Instead, give every custom word a realistic count before the run:

1. **Web counts** where they exist: about half of such words are in Norvig's
   `count_1w.txt` (lol 16.1M, app 12.2M, plaintext 456k, ciphertext 220k), on the same
   scale as `txt/en/unigrams.txt`.
2. **GPT-2 estimates** for the rest (too new for that corpus, such as github, chatgpt,
   bitcoin and emoji, or too rare, such as kasiski and playfair): the log
   probability of the word in a few neutral contexts, mapped to a count by a fit of GPT-2's
   estimate against the real counts of a few thousand dictionary words. About a second for
   a few hundred words.
3. **Trigram counts:** recount the subtitle corpus with the custom words included
   (`tools/count_trigrams.cpp`, about 2 minutes), so pairs like "the vigenere cipher" get
   counts where the corpus has them; otherwise the model backs off to the counts of 1-2.

The word file would take an optional count per line (`github 1500000`), and a command would
fill the counts in with 1-2. An existing index is then updated with `rerank` (about a
minute, no new search) and `rescore` (GPT-2 on the new shortlist).

## More languages, and benchmarks for them

German and Ukrainian are tested on a few known plaintexts only; `bench/realistic.py` has
English phrases. A phrase set per language would measure their ranking as for English.
Ukrainian readings grow fast with the key length (85 million at key length 10 on a
19-letter text, against 2.4 million up to key length 9): its nine 1-letter words fit almost
anywhere. Fewer or rarer short words, or a check that 1-letter words are not adjacent, would
cut that; both change what the search can find, so they need measuring first.

## Faster index building

- The forward pass of the column DP is single-threaded, and now makes two passes per column
  (one for the per-track state tables, one for the compact codes); each column can be
  split across threads. With the 81k-word dictionary, key length 12 on 25 letters takes
  18-21 minutes.

## Out of scope (recorded so they are not revisited)

- **Ciphertexts that repeat with the key's period.** The plaintext must then repeat too,
  and any repeating English text is an equally good answer, so the key length carries no
  information. `crack` skips such key lengths with a note.
- **Key lengths with more candidates than `--max-candidates`.** With a key nearly as long as
  the text (e.g. key length 9 on 10 letters), almost any word sequence fits; listing them
  is enumerating English. `crack` skips such key lengths with a note. The limit (default
  1 billion) bounds disk space and time, not memory, and can be raised.
- **Folding the language model into the search** (pruning unlikely key prefixes). It would
  lose the guarantee that the true plaintext is among the candidates; the search stays
  exhaustive and the models only rank.
- **A key prior favouring English-word keys.** Every key is scored as random letters
  (−key_length · ln 26). Preferring keys that are words would settle cases like "take the
  last train" (`important`) vs "take the fast train" (`importatt`), but the ranking is meant
  to judge the plaintext, not guess how keys are chosen.
