#pragma once

void print_format (char *exec_name);
bool read_file (const char *filename, std::string &buffer);
void format_buffer (const std::string &buffer, std::string &formatted);
bool is_string_correct(const std::string &str);