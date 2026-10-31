CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -funroll-loops -flto=auto \
           -Wall -Wextra -Wpedantic -pthread
SRCS     = src/dp.cpp src/index.cpp
HDRS     = src/tables.hpp src/cipher.hpp src/dp.hpp src/index.hpp
BIN      = bin/vigenere
TEST_BIN = bin/test_dp

all: bin $(BIN)

bin:
	mkdir -p bin

$(BIN): src/main.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(SRCS)

$(TEST_BIN): tests/test_dp.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_dp.cpp $(SRCS)

clean:
	rm -f $(BIN) $(TEST_BIN)

test: $(BIN) $(TEST_BIN)
	$(TEST_BIN)
	bash tests/run_tests.sh

.PHONY: all clean test
