# Build configurations live side by side in build/<config>:
#   release  -O2, the default
#   debug    -O0 -g
#   asan     AddressSanitizer + UndefinedBehaviorSanitizer
#   tsan     ThreadSanitizer
# e.g. `make BUILD=debug test`. Header dependencies are tracked automatically.

CXX   ?= g++
BUILD ?= release
OUT   := build/$(BUILD)

CPPFLAGS := -Iinclude -MMD -MP
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wshadow
LDFLAGS  := -pthread

ifeq ($(BUILD),release)
  CXXFLAGS += -O2 -g -DNDEBUG
else ifeq ($(BUILD),debug)
  CXXFLAGS += -O0 -g
else ifeq ($(BUILD),asan)
  CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
  LDFLAGS  += -fsanitize=address,undefined
else ifeq ($(BUILD),tsan)
  CXXFLAGS += -O1 -g -fsanitize=thread
  LDFLAGS  += -fsanitize=thread
else
  $(error unknown BUILD '$(BUILD)': use release, debug, asan or tsan)
endif

LIB_SRC  := $(wildcard src/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
APP_SRC  := $(wildcard apps/*.cpp)
BENCH_SRC := $(wildcard bench/*.cpp)

LIB_OBJ  := $(LIB_SRC:%.cpp=$(OUT)/%.o)
TEST_OBJ := $(TEST_SRC:%.cpp=$(OUT)/%.o)
APP_BIN  := $(APP_SRC:apps/%.cpp=$(OUT)/%)
BENCH_BIN := $(BENCH_SRC:bench/%.cpp=$(OUT)/%)
LIB      := $(OUT)/libob.a

.PHONY: all test test-asan test-tsan bench bench-e2e smoke check clean

all: $(OUT)/ob_tests $(APP_BIN) $(BENCH_BIN)

$(OUT)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	ar rcs $@ $^

$(OUT)/ob_tests: $(TEST_OBJ) $(LIB)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(OUT)/%: $(OUT)/apps/%.o $(LIB)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(OUT)/%: $(OUT)/bench/%.o $(LIB)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

# Unit, behaviour and differential tests. Pass a name filter with T=..., e.g. make test T=modify
test: $(OUT)/ob_tests
	./$(OUT)/ob_tests $(T)

test-asan:
	$(MAKE) BUILD=asan test

test-tsan:
	$(MAKE) BUILD=tsan test

bench:
	$(MAKE) BUILD=release all
	./build/release/bench_book

# Server + load generator over loopback TCP, with and without the journal.
bench-e2e:
	$(MAKE) BUILD=release all
	./scripts/bench_e2e.sh build/release

# End-to-end: start the server, drive it over TCP, check replies and recovery.
smoke:
	$(MAKE) BUILD=release all
	python3 scripts/smoke_test.py build/release

# Everything CI runs.
check: test test-asan test-tsan smoke

clean:
	rm -rf build

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(APP_SRC:%.cpp=$(OUT)/%.d) $(BENCH_SRC:%.cpp=$(OUT)/%.d)
