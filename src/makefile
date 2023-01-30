CC = g++

NAME = vigenere

FLAGS = -Ofast -Wall -Wextra -Werror -o

DEBUG_FLAGS = -g -Wall -Wextra -Werror -o

SRCS = main.cpp encrypt.cpp decrypt.cpp stat_funcs.cpp utils.cpp

BIN = ../bin

TESTDIR = ../test_suite
TESTSCRIPT = test.sh

all: $(NAME)

$(NAME) :
	$(CC) $(FLAGS) $(NAME) $(SRCS) 
	mv $(NAME) $(BIN)

debug:
	$(CC) $(DEBUG_FLAGS) $(NAME) $(SRCS) 
	mv $(NAME) $(BIN)

clean:
	rm -f $(BIN)/$(NAME)

re: clean all

test:
	cd $(TESTDIR) && bash $(TESTSCRIPT)
