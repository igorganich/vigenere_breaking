#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include "utils.hpp"
#include "stat_funcs.hpp"
#include "encrypt.hpp"
#include "decrypt.hpp"

using namespace std;

bool is_verbose;

int main (int argc, char **argv)
{
	if (3 != argc)
	{
		print_format (argv[0]);
	}
	else
	{
		if (getenv ("VERBOSE") != NULL && strcmp (getenv ("VERBOSE"), "1") == 0)
		{
			is_verbose = true;
		}
		fill_tables ();
		if (string (argv[1]) == string ("-d"))
		{
			decrypt (argv[2]);
		}
		else if (string (argv[1]) == string ("-e"))
		{
			encrypt (argv[2]);
		}
		else
		{
			print_format (argv[0]);
		}
	}
	return 0;
}
