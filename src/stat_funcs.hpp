#pragma once

void fill_tables (void);
double index_of_coincidence (const std::string &text);
int guess_key_len (const std::string &buffer);
double fitness (const std::string &text);
bool check_if_words (const std::string &str);