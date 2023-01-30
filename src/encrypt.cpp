#include <iostream>
#include <string>
#include <cstring>
#include "main.hpp"
#include "utils.hpp"

extern char rev_table['z' + 1];
extern char alpha_table[ALPHA_SIZE * 2];

using namespace std;

static void print_encrypted_text (const string &formatted, const string &key)
{
	int strsize = formatted.size ();
	int keylen = key.length ();
	for (int i = 0, keyindex = key.length (); i < strsize; i++, keyindex++)
	{
		int alphapos = (int)rev_table[(int)formatted[i]] + (int)rev_table[(int)key[keyindex % keylen]];
		cout << alpha_table[alphapos];
	}
}

void encrypt (const char *filename)
{
	string buffer ("");
	string formatted ("");
	string key ("key");

	if (false == read_file (filename, buffer))
	{
		cerr << "Can't read file\n";
	}
	else if (buffer.empty ())
	{
		cerr << "file is empty\n";
	}
	else
	{
		format_buffer (buffer, formatted);
		cerr << "enter encrypt key\n";
		cin >> key;
		if (true == is_string_correct (key))
		{
			print_encrypted_text (formatted, key);
		}
	}
}