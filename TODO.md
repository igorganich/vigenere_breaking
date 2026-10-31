# TODO

## Ranking

With GPT-2 rescoring the trigram's best 1,000, the true plaintext comes first for 96% of the
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

## Custom words: `--dict FILE`

A plaintext is only found if every word is in the dictionary. The list covers more than
expected ("dont", "london", "email"), but misses e.g. "usa", "covid", "smartphone", "wifi"
and American "color". An option to add a file of extra words (names, domain terms) would
cover most gaps; the language model would need a probability for them (e.g. that of the
rarest listed word). A much larger dictionary grows the search, so this is meant for a few
hundred words, not a 300,000-word list.

## Faster index building

- The forward pass of the column DP is single-threaded (about 1 min of the 3 min for the
  stress case); each column can be split across threads.
- The DP columns are now the only part of the build that grows in memory (candidates are on
  disk). States are 8 bytes per track plus padding; a compact encoding (17-bit trie nodes)
  would cut that by about half.

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
