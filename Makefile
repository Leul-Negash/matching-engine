CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude
DEBUG    := -std=c++20 -O0 -g -fsanitize=address,undefined -Wall -Wextra -Iinclude

SRC := src/orderbook.cpp

.PHONY: test test-asan bench clean

build/test: $(SRC) tests/test_orderbook.cpp include/orderbook.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) tests/test_orderbook.cpp -o $@

test: build/test
	./build/test

# Same tests with AddressSanitizer + UBSan: catches use-after-free, leaks, overflow.
test-asan:
	@mkdir -p build
	$(CXX) $(DEBUG) $(SRC) tests/test_orderbook.cpp -o build/test-asan
	./build/test-asan

build/bench: $(SRC) bench/bench.cpp include/orderbook.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DNDEBUG $(SRC) bench/bench.cpp -o $@

bench: build/bench
	./build/bench

clean:
	rm -rf build
