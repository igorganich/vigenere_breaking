#!/usr/bin/env python3
"""Build the English dictionary, txt/en/words.txt, in order of frequency.

Input: BASE, the 44,274-word corncob list this list was built from (txt/english_words.txt
as of commit 719342e: `git show 719342e:txt/english_words.txt > base.txt`), and in DIR:
  american-english-large   SCOWL size 70, Debian package wamerican-large 2020.12.07
  american-english-huge    SCOWL size 80, Debian package wamerican-huge 2020.12.07
  count_1w.txt             Peter Norvig's word counts (https://norvig.com/ngrams/)

The list is made of:
  - every word of 2-3 letters of BASE, and no others:
    the short words SCOWL adds are almost all abbreviations ("pm", "cd", "ltd"), and every
    extra short word multiplies the readings of a ciphertext;
  - every word of 4+ letters of BASE;
  - SCOWL-large words of 4+ letters, all lowercase, that occur in count_1w;
  - SCOWL-large names (capitalized) of 4+ letters seen at least NAME_MIN times in count_1w;
  - SCOWL-huge words of 4+ letters seen at least DERIVED_MIN times in count_1w that are a
    known prefix or suffix applied to a word already in the list ("reoperation" from
    "operation"), for the derived forms SCOWL-large leaves out;
  - the words of ALWAYS, which neither source has ("vigenere", the cipher this tool breaks).
Words are ordered by their count in count_1w; words it lacks come last, in BASE order.
The result is written to txt/en/words.txt.

Usage: python3 tools/make_word_list.py BASE DIR
  then rebuild the counts: tools/make_ngram_tables.py and tools/count_trigrams.cpp (README).
"""
import os, sys

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORDS = os.path.join(ROOT, "txt", "en", "words.txt")   # output

ALWAYS      = ["vigenere"]
NAME_MIN    = 100_000
DERIVED_MIN = 100_000
PREFIXES = ["un", "re", "in", "im", "il", "ir", "dis", "non", "pre", "mis", "over", "under", "out",
            "anti", "sub", "super", "inter", "semi", "self", "co", "de", "fore", "post", "counter"]
SUFFIXES = ["s", "es", "ed", "d", "ing", "er", "ers", "est", "ly", "ness", "ful", "less", "able",
            "ible", "ment", "ments", "ion", "ions", "ity", "ism", "ist", "ists", "ize", "ized",
            "izes", "izing", "al", "ally", "ish", "y", "ies", "ied", "ier", "iest", "ily", "iness"]


def read_scowl(path):
    """(lowercase words, names lowercased) of letters a-z only."""
    words, names = set(), set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            w = line.strip()
            if w.isascii() and w.isalpha():
                (words if w.islower() else names).add(w.lower())
    return words, names


def stems(w):
    """Words that `w` could be derived from by one prefix or suffix."""
    for s in SUFFIXES:
        if w.endswith(s) and len(w) - len(s) >= 3:
            st = w[:-len(s)]
            yield st
            yield st + "e"                                    # make + ing -> making
            if st[-1] == st[-2]:
                yield st[:-1]                                 # stop + ed -> stopped
            if s[0] == "i":
                yield st + "y"                                # happy + ness -> happiness
    for p in PREFIXES:
        if w.startswith(p) and len(w) - len(p) >= 3:
            yield w[len(p):]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, src = sys.argv[1], sys.argv[2]
    with open(base) as f:
        old = [w.strip() for w in f if w.strip()]
    count = {}
    with open(os.path.join(src, "count_1w.txt")) as f:
        for line in f:
            w, c = line.split("\t")
            count[w] = int(c)
    large, large_names = read_scowl(os.path.join(src, "american-english-large"))
    huge, _ = read_scowl(os.path.join(src, "american-english-huge"))

    words = set(old)
    words |= {w for w in large if len(w) >= 4 and w in count}
    names = {w for w in large_names if len(w) >= 4 and count.get(w, 0) >= NAME_MIN} - words
    words |= names
    derived = {w for w in huge - words
               if len(w) >= 4 and count.get(w, 0) >= DERIVED_MIN and any(s in words for s in stems(w))}
    words |= derived
    words |= set(ALWAYS)

    pos = {w: i for i, w in enumerate(old)}
    order = sorted(words, key=lambda w: (-count.get(w, 0), pos.get(w, len(old)), w))
    with open(WORDS, "w") as f:
        f.write("\n".join(order) + "\n")
    print(f"{len(order)} words: {len(old)} from BASE, {len(names)} names, "
          f"{len(derived)} derived forms, the rest from SCOWL-large "
          f"({sum(len(w) <= 3 for w in order)} of 2-3 letters)", file=sys.stderr)


if __name__ == "__main__":
    main()
