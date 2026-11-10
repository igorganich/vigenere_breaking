#!/usr/bin/env python3
"""Build the Ukrainian dictionary in txt/uk/: the word list, its counts and display forms.

Ukrainian plaintexts are written in the 33 letters of the Ukrainian alphabet
(абвгґдеєжзиіїйклмнопрстуфхцчшщьюя; Vigenère mod 33), lowercase, apostrophes dropped
("м'ясо" -> "мясо").

Input:
  COUNTS  word counts of the Ukrainian corpora from tools/count_words.cpp:
          "word count display", most frequent first
  SPELL   the Ukrainian spelling list of Debian's wukrainian, /usr/share/dict/ukrainian

A word is kept if the spelling list has it (lowercased, apostrophes dropped) and
  - 4+ letters: it occurs at least MIN_COUNT times in the corpora;
  - 3 letters: at least SHORT_MIN times, and its usual spelling is lowercase (not "США",
    not a name such as "Ян");
  - 2 letters: it is in TWO_LETTER (chosen by hand; abbreviations such as "км", "ЄС" are
    in the spelling list too).
The words of ALWAYS are added, which the corpora lack ("віженер", the cipher this tool
breaks), with the smallest count of the list.  The 1-letter words (і, в, з, у, а, я, є, й, о)
are set in src/lang.hpp; their counts are
written to unigrams.txt.  Short words are chosen strictly because each one multiplies the
readings of a ciphertext.

Output (txt/uk/):
  words.txt     the words of 2+ letters, most frequent first
  unigrams.txt  "word count", the same words and the 1-letter words
  display.txt   "word Spelling": the word's usual spelling in mid-sentence ("мясо м'ясо"),
                for the sentences GPT-2 reads

Usage: python3 tools/make_word_list_uk.py COUNTS SPELL
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "txt", "uk")

LETTERS    = "абвгґдеєжзиіїйклмнопрстуфхцчшщьюя"
MIN_COUNT  = 20
SHORT_MIN  = 2000
ALWAYS     = {"віженер": "Віженер", "віженера": "Віженера"}     # word -> spelling for GPT-2
SINGLES    = ["і", "в", "з", "у", "а", "я", "є", "й", "о"]          # as in src/lang.hpp
TWO_LETTER = ["на", "не", "що", "та", "до", "це", "за", "як", "ми", "їх", "її", "ви", "із", "по",
              "чи", "ще", "те", "то", "де", "ти", "зі", "ці", "ні", "бо", "їм", "ця", "цю", "ті",
              "би", "же", "ну", "аж", "ту", "об", "ой", "ах", "ох", "їй", "от"]


def norm(w):
    w = "".join(c for c in w.lower() if c not in "'’ʼ")
    return w if w and all(c in LETTERS for c in w) else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    counts_path, spell_path = sys.argv[1:]
    with open(spell_path, encoding="utf-8") as f:
        spell = {t for t in (norm(line.strip()) for line in f) if t}
    kept, singles = [], {}
    with open(counts_path, encoding="utf-8") as f:
        for line in f:
            word, count, display = line.split()
            count = int(count)
            n = len(word)
            if n == 1:
                if word in SINGLES: singles[word] = (count, display.lower())
                continue
            if n >= 3 and word not in spell:
                continue
            if n >= 4:
                ok = count >= MIN_COUNT
            elif n == 3:
                ok = count >= SHORT_MIN and display == display.lower()
            else:
                ok = word in TWO_LETTER
            if ok:
                kept.append((word, count, display))
    floor = min(c for _, c, _ in kept)
    have = {w for w, _, _ in kept}
    kept += [(w, floor, d) for w, d in ALWAYS.items() if w not in have]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "words.txt"), "w", encoding="utf-8") as f:
        f.write("".join(f"{w}\n" for w, _, _ in kept))
    with open(os.path.join(OUT, "unigrams.txt"), "w", encoding="utf-8") as f:
        f.write("".join(f"{w} {c}\n" for w, (c, _) in singles.items()))
        f.write("".join(f"{w} {c}\n" for w, c, _ in kept))
    with open(os.path.join(OUT, "display.txt"), "w", encoding="utf-8") as f:
        f.write("".join(f"{w} {d}\n" for w, (_, d) in singles.items()))
        f.write("".join(f"{w} {d}\n" for w, _, d in kept))
    missing = [w for w in SINGLES + TWO_LETTER if w not in singles and w not in {k[0] for k in kept}]
    print(f"{len(kept)} words of 2+ letters ({sum(len(w) == 2 for w, _, _ in kept)} of 2, "
          f"{sum(len(w) == 3 for w, _, _ in kept)} of 3), {len(singles)} of 1 letter"
          + (f"; not in the corpora: {' '.join(missing)}" if missing else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
