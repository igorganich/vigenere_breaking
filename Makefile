CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -funroll-loops -flto=auto \
           -Wall -Wextra -Wpedantic -pthread
SRCS     = src/dp.cpp src/index.cpp src/gpt2.cpp src/rescore.cpp
HDRS     = src/tables.hpp src/model.hpp src/cipher.hpp src/dp.hpp src/index.hpp src/gpt2.hpp src/rescore.hpp
BIN      = bin/vigenere
TEST_BIN = bin/test_dp
TEST_LM  = bin/test_gpt2

all: bin $(BIN)

bin:
	mkdir -p bin

$(BIN): src/main.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(SRCS)

$(TEST_BIN): tests/test_dp.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_dp.cpp $(SRCS)

$(TEST_LM): tests/test_gpt2.cpp src/gpt2.cpp src/dp.cpp src/gpt2.hpp src/dp.hpp src/tables.hpp
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_gpt2.cpp src/gpt2.cpp src/dp.cpp

clean:
	rm -f $(BIN) $(TEST_BIN) $(TEST_LM)

test: $(BIN) $(TEST_BIN) $(TEST_LM)
	$(TEST_BIN)
	$(TEST_LM)
	bash tests/run_tests.sh

.PHONY: all clean test
