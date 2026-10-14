# High-Performance Key-Value Store
#
#   make            build the library, kvcli, kvbench and the test binary
#   make test       build and run the test suite
#   make bench      run the benchmark (release build)
#   make test SAN=address     run tests under AddressSanitizer
#   make test SAN=thread      run tests under ThreadSanitizer
#   make clean

CXX      ?= c++
SAN      ?=

BUILD    := build$(if $(SAN),-$(SAN))
OPT      := $(if $(SAN),-O1 -g -fsanitize=$(SAN) -fno-omit-frame-pointer,-O2)
CXXFLAGS ?=
CXXFLAGS += -std=c++17 $(OPT) -Wall -Wextra -Wpedantic -pthread -Iinclude -Isrc -MMD -MP
LDFLAGS  += -pthread $(if $(SAN),-fsanitize=$(SAN))

LIB_SRCS   := $(wildcard src/*.cpp)
LIB_OBJS   := $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
TEST_SRCS  := $(wildcard tests/*.cpp)
TEST_OBJS  := $(TEST_SRCS:%.cpp=$(BUILD)/%.o)

LIB        := $(BUILD)/libkvstore.a
TEST_BIN   := $(BUILD)/kv_tests
CLI_BIN    := $(BUILD)/kvcli
BENCH_BIN  := $(BUILD)/kvbench

.PHONY: all test bench clean
all: $(LIB) $(CLI_BIN) $(BENCH_BIN) $(TEST_BIN)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(TEST_OBJS) $(LIB) $(LDFLAGS) -o $@

$(CLI_BIN): $(BUILD)/tools/kvcli.o $(LIB)
	$(CXX) $^ $(LDFLAGS) -o $@

$(BENCH_BIN): $(BUILD)/bench/kvbench.o $(LIB)
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

bench: $(BENCH_BIN)
	$(BENCH_BIN)

clean:
	rm -rf build build-address build-thread build-undefined

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BUILD)/tools/kvcli.d $(BUILD)/bench/kvbench.d
