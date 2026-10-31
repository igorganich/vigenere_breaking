# TODO

## Ranking

`trigram` is the default; `bigram` and `unigram` remain for comparison, and `rerank` switches
an existing index. The earlier `zipf` model was dropped after the realistic benchmark. On the
stress case (`bench/stress.sh`, 25 letters, key length 12) the true plaintext is at rank 9
(bigram 512, zipf 4,980). Open: finish the bigram/trigram comparison on all 105 phrases of
`bench/realistic.py`.

Further ideas, in the order they look most useful:
- **Sentence start.** Most messages start a sentence; P(w₁ | start) and P(w₂ | start w₁)
  from the subtitle corpus would favour "i am ..." over "a jury ...".
- **Key prior.** −key_length · ln 26 assumes random keys. Keys that are English words
  (`map`, `whilst`) could get a mixture prior, P(key) = ½ · 26^−L + ½ · P_word(key).
- **Larger trigram table.** 978k triples (count ≥ 15, 18 MB) from 179M words; more corpus
  and a lower cutoff raise coverage of real phrases (the table holds 149 of the 211 word
  triples in `bench/phrases.txt`) at the cost of a bigger file.

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
