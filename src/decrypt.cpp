#include <string>
#include <limits>
#include <iostream>
#include "main.hpp"
#include "stat_funcs.hpp"
#include "utils.hpp"

extern char alpha_table[ALPHA_SIZE * 2];
extern std::string dict[DICT_SIZE];

extern bool is_verbose;

using namespace std;

void transform_decrypt (string &plain, const string &cypher, const string &key)
{
	char *decrypt_table = (char *)alpha_table + ALPHA_SIZE;
	int cyplen = cypher.length ();
	int keylen = key.length ();

	plain = cypher;
	for (int i = 0, curkey = key.length (); i < cyplen; i++, curkey++)
	{
		plain[i] = decrypt_table[cypher[i] - key[curkey % keylen]];
	}
}

static bool crack_by_variational (const string &cypher, const string &cutcypher, int keylen)
{
	bool ret = true;
	string key ("");
	string tempkey ("");
	string tempplain ("");
	double bestfitness = std::numeric_limits<double>::lowest();
	double curfitness = 0;
	int keyindex = 0;

	if (0 == keylen)
	{
		ret = false;
	}
	else
	{
		key = string (keylen, 'a');
		for (int i = 0; i < VARIATIONAL_ITERATIONS * keylen; i++)
		{
			keyindex = (rand () + keylen) % keylen;
			tempkey = key;
			for (char k = 'a'; k <= 'z'; k++)
			{
				tempkey[keyindex] = k;
				transform_decrypt (tempplain, cutcypher, tempkey);
				curfitness = fitness (tempplain);
				if (curfitness > bestfitness)
				{
					bestfitness = curfitness;
					key = tempkey;
				}
			}
		}
		transform_decrypt (tempplain, cutcypher, key);
		ret = check_if_words (tempplain);
	}
	if (true == ret)
	{
		transform_decrypt (tempplain, cypher, key);
		cout << tempplain << endl;
		cout << "key is " << key << endl;
	}
	else
	{
		if (true == is_verbose)
		{
			cerr << "can't crack by variational\n";
		}
	}
	return ret;
}

static bool process_dict_crack (const string &cypher, const string &cutcypher, const size_t keylen)
{
	bool ret = false;
	char *key_table = (char *)alpha_table + ALPHA_SIZE;
	string curkey ("");
	double bestfitness[CANDIDATES_SIZE];
	double curfitness = 0;
	const size_t textlen = cutcypher.length ();
	size_t random = 0;
	size_t keyindex = 0;
	string tempplain ("");
	string bestkeys[CANDIDATES_SIZE];
	int start = 0;

	for (int i = 0; i < CANDIDATES_SIZE; i++)
	{
		bestkeys[i] = string (keylen, 'a');
		bestfitness[i] = std::numeric_limits<double>::lowest();
	}
	for (int i = 0; i < PROC_DICT_ITERS; i++)
	{
		if (i % 100 == 0)
		{
			if (true == is_verbose)
			{
				cerr << ((double)i / PROC_DICT_ITERS) * 100 << "%" <<  endl;
			}
		}
		for (int bkeyind = 0; bkeyind < CANDIDATES_SIZE; bkeyind++)
		{
			curkey = bestkeys[bkeyind];
			random = (rand() + textlen) % textlen;
			keyindex = (random + keylen) % keylen; 
			for (const string &word : dict)
			{
				if (word.length () + random >= textlen)
				{
					continue;
				}
				for (size_t k = keyindex, j = 0; k < keylen && j < word.length (); k++, j++)
				{
					curkey[k] = key_table[cutcypher[random + j] - word[j]];
				}
				transform_decrypt (tempplain, cutcypher, curkey);
				curfitness = fitness (tempplain);
				if (curfitness > bestfitness[CANDIDATES_SIZE - 1])
				{
					for (start = 0; start < CANDIDATES_SIZE; start++)
					{
						if (curfitness >= bestfitness[start])
						{
							if (curfitness == bestfitness[start])
							{
								start = -1;
							}
							break;
						}
					}
					if (start != -1)
					{
						for (int fitindex = CANDIDATES_SIZE - 1; fitindex > start; fitindex--)
						{
							bestfitness[fitindex] = bestfitness[fitindex - 1];
							bestkeys[fitindex] = bestkeys[fitindex - 1];
						}
						bestfitness[start] = curfitness;
						bestkeys[start] = curkey;
					}
				}
			}
		}
	}
	for (int i = 0; i < CANDIDATES_SIZE; i++)
	{
		transform_decrypt (tempplain, cypher, bestkeys[i]);
		if (true == is_verbose)
		{
			cerr << "candidate\n" << tempplain << endl;
		}
		ret = check_if_words (tempplain);
		if (true == ret)
		{
			cout << tempplain << endl;
			cout << "key is " << bestkeys[i] << endl;
			break;
		}
	}
	return ret;
}

static bool crack_by_dict (const string &cypher)
{
	bool ret = false;
	int keylen = 0;
	string retkey ("");
	string tempplain ("");
	string cutcypher = (cypher.length () < SMALL_SUB_CYPHER_SIZE) ? 
							cypher : 
							cypher.substr (0, SMALL_SUB_CYPHER_SIZE);

	for (keylen = 1; keylen <= MAX_KEY_LEN && false == ret; keylen++)
	{
		if (true == is_verbose)
		{
			cerr << "checking keylen = " << keylen << endl;
		}
		ret = process_dict_crack (cypher, cutcypher, keylen);
	}
	if (false == ret)
	{
		if (true == is_verbose)
		{
			cerr << "can't crack by dictionary variational\n";
		}
	}
	return ret;
}

static void crack_cyphertext (const string &buffer)
{
	string cutcypher = (buffer.length () < BIG_SUB_CYPHER_SIZE) ? 
							buffer : 
							buffer.substr (0, BIG_SUB_CYPHER_SIZE);
	int keylen = guess_key_len (cutcypher);
	int ret = false;

	srand((unsigned) time(NULL));
	ret = crack_by_variational (buffer, cutcypher, keylen);
	if (false == ret)
	{
		ret = crack_by_dict (buffer);
	}
	if (true == ret)
	{
		if (true == is_verbose)
		{
			cerr << "success\n";
		}
	}
	else
	{
		cout << "can't crack\n";
	}
}

void decrypt (const char *filename)
{
	string buffer ("");

	if (false == read_file (filename, buffer))
    {
        cerr << "Can't read file\n";
    }
	else if (buffer.empty ())
    {
		cerr << "file is empty\n";
    }
	else if (buffer.length () <= MAX_KEY_LEN)
	{
		cerr << "Cyphertext length is not enough to crack it\n";
	}
	else
	{
		if (true == is_string_correct (buffer))
		{
			crack_cyphertext (buffer);
		}
	}
}