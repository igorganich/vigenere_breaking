#pragma once
#include <string>
#include <utility>
#include <vector>

// ── Plaintext languages ───────────────────────────────────────────────────────
//
// Every language is written in the cipher alphabet a-z.  Its data lives in txt/<code>/:
//   words.txt                        the word list, most frequent first
//   unigrams.txt, bigrams.txt,       counts for the ranking models (model.hpp)
//   trigrams.txt, trigram_contexts.txt
//   display.txt (optional)           "word Spelling": how GPT-2 should read a word, for
//                                    languages whose spelling the cipher alphabet loses
//                                    (German "schoen" -> "schön", "haus" -> "Haus")
// and its GPT-2 in models/<gpt2_dir>/.

struct Language {
    std::string code, name;
    std::string gpt2_dir;                                  // under models/
    std::vector<std::pair<std::string, int>> singles;     // 1-letter words and their ranks
    std::string get_gpt2;                                  // how to download the GPT-2
};

inline const std::vector<Language>& languages()
{
    static const std::vector<Language> all = {
        // German umlauts and ß are written ae, oe, ue, ss.
        {"en", "English", "gpt2",    {{"a", 5}, {"i", 11}}, "tools/get_gpt2.sh"},
        {"de", "German",  "gpt2-de", {},                    "tools/get_gpt2.sh de"},
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
