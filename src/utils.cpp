#include <iostream>
#include <fstream>
#include "utils.hpp"

using namespace std;

void print_format (char *exec_name)
{
	cerr << "Format: " << exec_name << " -[ed] filename" << endl;
}

bool read_file (const char *filename, string &buffer)
{
	size_t size = 0;
	bool ret = false;
	ifstream file (filename);

	if (file)
	{
		file.seekg (0, ios::end);
		size = file.tellg ();
		buffer = string (size, ' ');
		file.seekg (0);
	        file.read (&buffer[0], size);
	}
	ret = file.good ();
	file.close ();
	return (ret);
}

void format_buffer (const string &buffer, string &formatted)
{
	int cur = 0;
	char lowercase = 0;

	formatted = string (buffer.length (), ' ');
	for (const char& c : buffer) 
	{
		if (isalpha (c))
		{
			lowercase = tolower (c);
			if ('\0' != lowercase)
			{
				formatted[cur] = lowercase;
				cur++;
			}
		}
	}
	formatted.resize (cur);
}

bool is_string_correct(const string &str)
{
	bool ret = true;

	for (const char& c : str)
	{
		if (!isalpha (c) || !islower (c))
		{
			cerr << "Input should consists of alphabet characters in lowercase.\n";
			cerr << "Bad character: " << c << endl;
			ret = false;
			break;
		}
	}
	return ret;
}