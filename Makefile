CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -funroll-loops -flto \
           -Wall -Wextra -Wpedantic
SRCS     = src/crack.cpp src/main.cpp
HDRS     = src/tables.hpp src/cipher.hpp src/crack.hpp
BIN      = bin/vigenere

all: bin $(BIN)

bin:
	mkdir -p bin

$(BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(BIN)

test: $(BIN)
	cd tests && bash run_tests.sh

.PHONY: all clean test
