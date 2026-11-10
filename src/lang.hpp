#pragma once
#include "tables.hpp"
#include <string>
#include <utility>
#include <vector>

// ── Plaintext languages ───────────────────────────────────────────────────────
//
// English and German are written in the 26 letters a-z (German umlauts and ß as ae, oe, ue,
// ss), Ukrainian in its own 33 letters (Vigenère mod 33).  A language's data lives in
// txt/<code>/, in UTF-8:
//   words.txt                        the word list, most frequent first
//   unigrams.txt, bigrams.txt,       counts for the ranking models (model.hpp)
//   trigrams.txt, trigram_contexts.txt
//   display.txt (optional)           "word Spelling": how GPT-2 should read a word, for
//                                    languages whose spelling the cipher alphabet loses
//                                    (German "schoen" -> "schön", "haus" -> "Haus",
//                                    Ukrainian "мясо" -> "м'ясо")
// and its GPT-2 in models/<gpt2_dir>/.

struct Language {
    std::string code, name;
    Alphabet    alphabet;
    std::string gpt2_dir;                                  // under models/
    std::vector<std::pair<std::string, int>> singles;     // 1-letter words (UTF-8) and their ranks
    std::string get_gpt2;                                  // how to download the GPT-2
};

inline const std::vector<Language>& languages()
{
    static const std::vector<Language> all = {
        {"en", "English", Alphabet::latin(), "gpt2", {{"a", 5}, {"i", 11}}, "tools/get_gpt2.sh"},
        {"de", "German",  Alphabet::latin(), "gpt2-de", {}, "tools/get_gpt2.sh de"},
        {"uk", "Ukrainian",
         Alphabet::of("абвгґдеєжзиіїйклмнопрстуфхцчшщьюя", "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"), "gpt2-uk",
         {{"і", 1}, {"в", 2}, {"з", 4}, {"у", 6}, {"а", 9}, {"я", 15}, {"є", 18}, {"й", 34}, {"о", 120}},
         "tools/get_gpt2.sh uk"},
    };
    return all;
}

// The language with this code, or nullptr.
inline const Language* find_language(const std::string& code)
{
    for (const auto& l : languages()) if (l.code == code) return &l;
    return nullptr;
}

inline std::string language_codes()
{
    std::string s;
    for (const auto& l : languages()) s += (s.empty() ? "" : ", ") + l.code;
    return s;
}
