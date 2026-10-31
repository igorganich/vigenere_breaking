# TODO

## Ranking

The trigram model ranks the true plaintext first for 95% of the phrases in
`bench/realistic.py`. Its misses are texts that are short for their key, where many fluent
readings fit and the key prior decides much of the order: "money talks" (10 letters, key
length 6) is 11th, "the coffee is too hot" (15 letters, key length 10) 4th, and the stress
case (`bench/stress.sh`, 25 letters, key length 12) 9th. Ideas, most promising first; each
can be measured on the cached searches with `python3 bench/realistic.py --cached-only`:

- **Sentence start.** Most messages start a sentence. P(w₁ | start) and P(w₂ | start w₁)
  from the subtitle corpus would favour "i am ..." over "a jury ...".
- **Key prior.** −key_length · ln 26 assumes random keys. Keys that are English words
  (`map`, `whilst`) could get a mixture prior, P(key) = ½ · 26^−L + ½ · P_word(key).
- **Larger trigram table.** 978k triples (count ≥ 15, 18 MB) from 179M words. More corpus
  and a lower cutoff raise coverage of real phrases (the table holds 149 of the 211 word
  triples in `bench/phrases.txt`), at the cost of a bigger file.
- **Harder benchmark cases.** Most phrases in `bench/phrases.txt` are now ranked first, so
  the benchmark separates models poorly. Cases with keys close to the text length would.

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
  exhaustive and the model only ranks.
