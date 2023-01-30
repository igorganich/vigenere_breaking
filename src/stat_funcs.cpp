#include <string>
#include <fstream>
#include <math.h>
#include "main.hpp"

using namespace std;

char rev_table['z' + 1] = {0};
char alpha_table[ALPHA_SIZE * 2] = {0};
double freq_table[ALPHA_SIZE][ALPHA_SIZE][ALPHA_SIZE][ALPHA_SIZE] = {0}; // frequency logarithms table for tetragrams
string dict[DICT_SIZE];

void fill_tables (void)
{
	string temp ("");
	int freq;
	ifstream freqs_file("../txt/english_quadgrams.txt");
	long int total = 0;
	ifstream dict_file("../txt/english_words.txt");
	int curdict = 0;

	for (char c = 'a'; c <= 'z'; c++)
	{
		rev_table[(int)c] = c - 'a';
        }
	for (char c = 'a'; c <= 'z'; c++)
	{
		alpha_table[c - 'a'] = c;
		alpha_table[c - 'a' + 26] = c;
	}
	
	while (freqs_file >> temp)
	{
		freqs_file >> freq;
		freq_table	[(int)rev_table[(int)temp[0]]]
				[(int)rev_table[(int)temp[1]]]
				[(int)rev_table[(int)temp[2]]]
				[(int)rev_table[(int)temp[3]]] = (double)freq;
		total += freq;
	}
	for (int i = 0; i < ALPHA_SIZE; i++)
		for (int j = 0; j < ALPHA_SIZE; j++)
			for (int k = 0; k < ALPHA_SIZE; k++)
				for (int l = 0; l < ALPHA_SIZE; l++)
				{
					if (freq_table[i][j][k][l] != 0)
					{
						freq_table[i][j][k][l] = log (freq_table[i][j][k][l] / total);
					}
					else
					{
						freq_table[i][j][k][l] = EMPTY_FREQ;
					}
				}
	freqs_file.close ();
	while (dict_file >> temp)
	{
		dict[curdict] = temp;
		curdict++;
	}
	dict_file.close ();
}

double index_of_coincidence (const string &text)
{
	int counts[ALPHA_SIZE] = {0};
	int numer = 0;
	int total = 0;
	double res = 0;
	for (const char& c : text)
	{
		counts [(int)rev_table[(int)c]] += 1;
	}
	for (int i = 0; i < ALPHA_SIZE; i++)
	{
		numer += counts[i] * (counts[i] - 1);
		total += counts[i];
	}
	res = (double)(ALPHA_SIZE * numer) / (double)(total * (total - 1));
	return res;
}

int guess_key_len (const string &buffer)
{
	bool is_found = false;
	int period = 0;
	int bufsize = buffer.length ();
	string slices[MAX_KEY_LEN + 1];
	double sum = 0;
	double ioc = 0;

	while (false == is_found && period <= MAX_KEY_LEN)
	{
		period++;
		for (int i = 0; i < period; i++)
		{
			slices[i] = string ("");
		}
		for (int i = 0; i < period; i++)
		{
			slices[i] += buffer[i];
		}
		for (int i = period; i < bufsize; i++)
		{
			slices[i % period] += buffer[i];
		}
		sum = 0;
		for (int i = 0; i < period; i++)
		{
			sum += index_of_coincidence (slices[i]);
		}
		ioc = sum / period;
		if (1.6 < ioc)
		{
			is_found = true;
		}
	}
	return false == is_found ? 0 : period;
}

double fitness (const string &text)
{
	double result = 0;
	double curfreqlog = 0;
	int cypsize = text.length () - 3;

	for (int i = 0; i < cypsize; i++)
	{
		curfreqlog = freq_table	[(int)rev_table[(int)text[i + 0]]]
					[(int)rev_table[(int)text[i + 1]]]
					[(int)rev_table[(int)text[i + 2]]]
					[(int)rev_table[(int)text[i + 3]]];
		result += curfreqlog;
	}
	result = result / cypsize;
	return result;
}

bool check_if_words (const string &str)
{
	bool ret = false;
	bool *charmap = new bool[str.length ()]();
	int counter = 0;
	size_t cur_pos = 0;

	for (string &word : dict)
	{
		cur_pos = 0;
		while ((cur_pos = str.find (word, cur_pos)) != string::npos)
		{
			for (size_t i = 0; i < word.length (); i++)
			{
				if (false == charmap[cur_pos + i])
				{
					charmap[cur_pos + i] = true;
					counter++;
				}
			}
			cur_pos += 1;
		}
	}
	if ((double)counter / str.length () > 0.95)
	{
		ret = true;
	}
	delete[] charmap;
	return ret;
}