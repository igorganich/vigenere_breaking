#!/usr/bin/env python3
"""Build the word-count tables the ranking models read, from Peter Norvig's n-gram counts.

Input (https://norvig.com/ngrams/, derived from the Google Web Trillion Word Corpus):
  count_1w.txt   word<TAB>count          333,333 most frequent words
  count_2w.txt   w1 w2<TAB>count         ~286,000 most frequent word pairs (counts >= 100,000)

Output, restricted to the English dictionary (txt/en/words.txt, plus "a" and "i"):
  txt/en/unigrams.txt        "word count", one line per dictionary word, in dictionary order.
                             Words missing from count_1w (mostly hyphen-stripped compounds
                             such as "brotherinlaw") get half the smallest count in count_1w.
  txt/en/bigrams.txt         "w1 w2 count" for pairs with both words in the dictionary.
Counts of entries that differ only in case are added together.

Usage: python3 tools/make_ngram_tables.py DIR_WITH_COUNT_FILES
"""
import os, sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TXT  = os.path.join(ROOT, "txt", "en")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    with open(os.path.join(TXT, "words.txt")) as f:
        words = [w.strip() for w in f if len(w.strip()) >= 2]
    words += [w for w in ("a", "i") if w not in words]
    vocab = set(words)

    c1 = Counter()
    with open(os.path.join(src, "count_1w.txt")) as f:
        for line in f:
            w, c = line.split("\t")
            c1[w.lower()] += int(c)
    floor = min(c1.values()) // 2
    with open(os.path.join(TXT, "unigrams.txt"), "w") as f:
        for w in words:
            f.write(f"{w} {c1.get(w, floor)}\n")

    c2 = Counter()
    with open(os.path.join(src, "count_2w.txt")) as f:
        for line in f:
            pair, c = line.rsplit("\t", 1)
            a, b = pair.lower().split(" ")
            if a in vocab and b in vocab:
                c2[a, b] += int(c)
    with open(os.path.join(TXT, "bigrams.txt"), "w") as f:
        for (a, b), c in sorted(c2.items()):
            f.write(f"{a} {b} {c}\n")
    print(f"{len(words)} words ({sum(w not in c1 for w in words)} at the floor count {floor}), "
          f"{len(c2)} word pairs", file=sys.stderr)


if __name__ == "__main__":
    main()
