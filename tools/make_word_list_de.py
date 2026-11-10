#!/usr/bin/env python3
"""Build the German dictionary in txt/de/: the word list, its counts and display forms.

German plaintexts are written in the 26 letters a-z, with umlauts and ß transliterated as
ciphers write them: ä -> ae, ö -> oe, ü -> ue, ß -> ss ("schön" -> "schoen").

Input:
  COUNTS  word counts of the German OpenSubtitles corpus from tools/count_words.cpp:
          "word count display", most frequent first
  SPELL   the German spelling list of Debian's wngerman (igerman98), /usr/share/dict/ngerman

A word is kept if the spelling list has it (after transliteration) and
  - 4+ letters: it occurs at least MIN_COUNT times in the corpus;
  - 3 letters: at least SHORT_MIN times, its usual spelling is not all capitals (FBI, SMS),
    and it is not in NOT_GERMAN (English words and names the spelling list has);
  - 2 letters: it is in TWO_LETTER (chosen by hand; "na", "ok", "ha" are not in the
    spelling list);
and the words of ALWAYS are added, which neither source has ("vigenere", the cipher this
tool breaks), with the smallest count of the list.
Short words are chosen strictly because each one multiplies the readings of a ciphertext.

Output (txt/de/):
  words.txt     the words, most frequent first
  unigrams.txt  "word count", in the same order
  display.txt   "word Spelling": the word's usual spelling in mid-sentence ("schoen schön",
                "haus Haus"), for the sentences GPT-2 reads

Usage: python3 tools/make_word_list_de.py COUNTS SPELL
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "txt", "de")

MIN_COUNT  = 20
SHORT_MIN  = 2000
TWO_LETTER = ["du", "es", "zu", "er", "in", "ja", "so", "an", "da", "um", "im", "wo", "oh", "am",
              "ab", "ob", "je", "au", "ei", "ex", "eh", "na", "ok", "ha"]
NOT_GERMAN = {"sir", "new", "sam", "tom", "max", "ben", "jim", "bob", "san", "red", "don", "van",
              "tim", "boy", "mac", "gus", "liz", "sun", "jan", "mio", "kai", "eli", "eva", "rom",
              "min"}
ALWAYS     = {"vigenere": "Vigenère"}     # word -> spelling for GPT-2
TRANSLIT = {"ä": "ae", "ö": "oe", "ü": "ue", "ß": "ss", "Ä": "ae", "Ö": "oe", "Ü": "ue"}


def translit(w):
    w = "".join(TRANSLIT.get(c, c) for c in w).lower()
    return w if w.isascii() and w.isalpha() else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    counts_path, spell_path = sys.argv[1:]
    with open(spell_path, encoding="utf-8", errors="replace") as f:
        spell = {t for t in (translit(line.strip()) for line in f) if t}
    kept = []                                 # (word, count, display), most frequent first
    with open(counts_path, encoding="utf-8") as f:
        for line in f:
            word, count, display = line.split()
            count = int(count)
            n = len(word)
            if n >= 3 and word not in spell:
                continue
            if n >= 4:
                ok = count >= MIN_COUNT
            elif n == 3:
                ok = count >= SHORT_MIN and not display.isupper() and word not in NOT_GERMAN
            else:
                ok = word in TWO_LETTER
            if ok:
                kept.append((word, count, display))
    floor = min(c for _, c, _ in kept)
    have = {w for w, _, _ in kept}
    kept += [(w, floor, d) for w, d in ALWAYS.items() if w not in have]
    missing = [w for w in TWO_LETTER if w not in {k[0] for k in kept}]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "words.txt"), "w") as f:
        f.write("".join(f"{w}\n" for w, _, _ in kept))
    with open(os.path.join(OUT, "unigrams.txt"), "w") as f:
        f.write("".join(f"{w} {c}\n" for w, c, _ in kept))
    with open(os.path.join(OUT, "display.txt"), "w", encoding="utf-8") as f:
        f.write("".join(f"{w} {d}\n" for w, _, d in kept))
    print(f"{len(kept)} words ({sum(len(w) == 2 for w, _, _ in kept)} of 2 letters, "
          f"{sum(len(w) == 3 for w, _, _ in kept)} of 3)"
          + (f"; 2-letter words not in the corpus: {' '.join(missing)}" if missing else ""),
          file=sys.stderr)


if __name__ == "__main__":
    main()
