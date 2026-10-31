# TODO

## Ranking: a word-pair model

The search is exhaustive, so finding the plaintext is no longer the problem; ranking it is.
Scores use unigram word frequencies estimated from rank, so word salad of common words
outranks real sentences. On the stress case (`bench/stress.sh`, 25 letters, key length 12)
the true plaintext is at rank 4,980 of 55.6 million, behind readings like "may employ
ushered triple tow".

A word-bigram model (log P(w₂ | w₁) from a corpus) would rank grammatical sequences far
higher. It needs a bigram table with counts; the current word list has ranks only. Scoring
stays a DP over the word split, with the previous word as extra state.

## Custom words: `--dict FILE`

Without stray letters, a plaintext is only found if every word is in the dictionary. The
list covers more than expected ("dont", "london", "email"), but misses e.g. "usa", "covid",
"smartphone", "wifi" and American "color". An option to add a file of extra words (names,
domain terms) would cover most gaps. A much larger dictionary grows the search, so this
is meant for a few hundred words, not a 300,000-word list.

## Faster index building

- The forward pass of the column DP is single-threaded (about 1 min of the 3.5 min for the
  stress case); each column can be split across threads.
- States are 8 bytes per track plus padding; a compact encoding (17-bit trie nodes)
  would cut memory per column by about half and allow longer columns before
  `--max-candidates` matters.

## Out of scope (recorded so they are not revisited)

- **Ciphertexts that repeat with the key's period.** The plaintext must then repeat too,
  and any repeating English text is an equally good answer, so the key length carries no
  information. `crack` skips such key lengths with a note.
- **Key lengths with more candidates than `--max-candidates`.** With a key nearly as long as
  the text (e.g. key length 9 on 10 letters), almost any word sequence fits; listing them
  is enumerating English. `crack` skips such key lengths with a note.
- **Disk space** for very large indexes (8 bytes per candidate).
