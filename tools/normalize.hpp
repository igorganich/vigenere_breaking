// Word normalization shared by tools/count_words.cpp and tools/count_trigrams.cpp: how a
// word of a corpus is written in a cipher alphabet.
//
//   - Latin letters are lowercased; German umlauts and ß are transliterated as German ciphers
//     write them (ä -> ae, ö -> oe, ü -> ue, ß -> ss).
//   - Ukrainian letters (а-я without ъ ы э ё, plus ґ є і ї) are lowercased and kept in UTF-8.
//   - Apostrophes (' ’ ʼ) are dropped ("don't" -> "dont", "м'ясо" -> "мясо").
//   - Any other letter (é, ñ, Russian ы ...) makes the word foreign: it is outside every list.
//   - Everything else separates words.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>

namespace norm {

enum Kind { Letter, Apostrophe, Foreign, Separator };

// Classify the character at line[i], advance i past it, and append its normalized form to
// `word` (for letters) and its original form to `orig` (letters and apostrophes, which a
// display spelling keeps; any apostrophe becomes ').
inline Kind next(const std::string& line, size_t& i, std::string& word, std::string& orig)
{
    const unsigned char c = (unsigned char)line[i];
    if (c < 0x80) {
        i++;
        if (std::isalpha(c)) { word += (char)std::tolower(c); orig += (char)c; return Letter; }
        if (c == '\'')       { orig += '\''; return Apostrophe; }
        return Separator;
    }
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + n > line.size()) n = line.size() - i;
    const std::string ch = line.substr(i, n);
    i += n;
    uint32_t cp = c;
    if (n == 2) cp = (c & 0x1F) << 6 | ((unsigned char)ch[1] & 0x3F);
    if (n == 3) cp = (c & 0x0F) << 12 | ((unsigned char)ch[1] & 0x3F) << 6 | ((unsigned char)ch[2] & 0x3F);
    if (n == 4) cp = (c & 0x07) << 18 | ((unsigned char)ch[1] & 0x3F) << 12 | ((unsigned char)ch[2] & 0x3F) << 6 |
                     ((unsigned char)ch[3] & 0x3F);
    auto utf8 = [](uint32_t x) {
        std::string s;
        if (x < 0x800) { s += (char)(0xC0 | x >> 6); s += (char)(0x80 | (x & 0x3F)); }
        else { s += (char)(0xE0 | x >> 12); s += (char)(0x80 | (x >> 6 & 0x3F)); s += (char)(0x80 | (x & 0x3F)); }
        return s;
    };
    switch (cp) {                                           // German
        case 0xE4: case 0xC4: word += "ae"; orig += ch; return Letter;   // ä Ä
        case 0xF6: case 0xD6: word += "oe"; orig += ch; return Letter;   // ö Ö
        case 0xFC: case 0xDC: word += "ue"; orig += ch; return Letter;   // ü Ü
        case 0xDF:            word += "ss"; orig += ch; return Letter;   // ß
        case 0x2019: case 0x2BC: orig += '\''; return Apostrophe;        // ’ ʼ
    }
    uint32_t lower = 0;                                     // Ukrainian
    if (cp >= 0x430 && cp <= 0x44F) lower = cp;             // а-я
    else if (cp >= 0x410 && cp <= 0x42F) lower = cp + 0x20; // А-Я
    else if (cp == 0x454 || cp == 0x456 || cp == 0x457 || cp == 0x491) lower = cp;   // є і ї ґ
    else if (cp == 0x404 || cp == 0x406 || cp == 0x407) lower = cp + 0x50;           // Є І Ї
    else if (cp == 0x490) lower = 0x491;                                             // Ґ
    if (lower == 0x44A || lower == 0x44B || lower == 0x44D) lower = 0;              // ъ ы э: Russian
    if (lower) { word += utf8(lower); orig += ch; return Letter; }
    // Other letters (Latin-1 letters, Cyrillic outside Ukrainian, other scripts) are foreign;
    // other symbols (punctuation, ×, ÷, «») separate words.
    const bool letter = (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) || (cp >= 0x370 && cp <= 0x52F) ||
                        (cp >= 0x0590 && cp <= 0x1FFF) || cp >= 0x3040;
    return letter ? Foreign : Separator;
}

} // namespace norm
