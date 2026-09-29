CC       ?= cc
CXX      ?= c++
CFLAGS   ?= -O2 -g
WARN      = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CPPFLAGS += -Iinclude
LDLIBS   += -lpthread -lm
BUILD    ?= build

SRC  = src/buf.c src/http.c src/json.c src/server.c src/static.c src/llm.c
OBJ  = $(SRC:src/%.c=$(BUILD)/%.o)
HDRS = $(wildcard include/*.h) src/internal.h
EX   = $(BUILD)/spored.o $(BUILD)/echo_backend.o

# Optional llama.cpp backend: make llama LLAMA_DIR=/path/to/llama.cpp
# LLAMA_DIR must hold include/llama.h and lib/lib{llama,ggml*}.a.
LLAMA_DIR  ?= ../cyllama/thirdparty/llama.cpp
LLAMA_LIBS  = $(LLAMA_DIR)/lib/libllama.a $(LLAMA_DIR)/lib/libggml.a \
              $(LLAMA_DIR)/lib/libggml-cpu.a $(LLAMA_DIR)/lib/libggml-base.a

.PHONY: all test unit integration replay asan tsan fuzz fuzz-run llama test-llama clean

all: $(BUILD)/libspore.a $(BUILD)/spored

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/%.o: examples/%.c $(HDRS) examples/echo_backend.h | $(BUILD)
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) -Iexamples -c $< -o $@

$(BUILD)/libspore.a: $(OBJ)
	$(AR) rcs $@ $^

$(BUILD)/spored: $(EX) $(BUILD)/libspore.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_unit: tests/test_unit.c $(BUILD)/libspore.a $(HDRS)
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) -Isrc $< $(BUILD)/libspore.a $(LDLIBS) -o $@

test: unit replay integration

unit: $(BUILD)/test_unit
	$(BUILD)/test_unit

integration: $(BUILD)/spored
	SPORED=$(BUILD)/spored uv run --no-project --with pytest --with openai pytest -q tests

# Seed corpus through the fuzz targets with any compiler; see `fuzz` below.
FUZZ_SRC = src/buf.c src/http.c src/json.c
$(BUILD)/replay_%: tests/fuzz/fuzz_%.c tests/fuzz/replay.c $(FUZZ_SRC) $(HDRS) | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra $(CFLAGS) $(CPPFLAGS) -Isrc $< tests/fuzz/replay.c \
		$(FUZZ_SRC) -lm -o $@

replay: $(BUILD)/replay_http $(BUILD)/replay_json
	$(BUILD)/replay_http tests/fuzz/seeds/http/*
	$(BUILD)/replay_json tests/fuzz/seeds/json/*

SAN = -O1 -g -fno-omit-frame-pointer
asan:
	$(MAKE) BUILD=build-asan CFLAGS="$(SAN) -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined" test
# setarch -R: TSan cannot map its shadow memory under high-entropy ASLR.
tsan:
	$(if $(shell command -v setarch),setarch $(shell uname -m) -R) \
	$(MAKE) BUILD=build-tsan CFLAGS="$(SAN) -fsanitize=thread" \
		LDFLAGS="-fsanitize=thread" test

# ---- libFuzzer (clang) ---------------------------------------------------
# `make fuzz-run FUZZ_TIME=600`. New corpus entries go to build-fuzz/corpus-*;
# copy crash reproducers into tests/fuzz/seeds/ so `make test` replays them.

FUZZ_CC   ?= clang
FUZZ_TIME ?= 60

build-fuzz/fuzz_%: tests/fuzz/fuzz_%.c $(FUZZ_SRC) $(HDRS)
	@mkdir -p build-fuzz/corpus-$*
	$(FUZZ_CC) -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined \
		-fno-sanitize-recover=undefined $(CPPFLAGS) -Isrc $< $(FUZZ_SRC) -lm -o $@

fuzz: build-fuzz/fuzz_http build-fuzz/fuzz_json

fuzz-run: fuzz
	for t in http json; do \
		build-fuzz/fuzz_$$t -max_total_time=$(FUZZ_TIME) -dict=tests/fuzz/$$t.dict \
			-artifact_prefix=build-fuzz/ build-fuzz/corpus-$$t tests/fuzz/seeds/$$t \
			|| exit 1; \
	done

# ---- llama.cpp backend --------------------------------------------------

$(BUILD)/spore_llama.o: backends/llama/spore_llama.cpp backends/llama/spore_llama.h $(HDRS) | $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra $(CFLAGS) $(CPPFLAGS) -Ibackends/llama \
		-I$(LLAMA_DIR)/include -c $< -o $@

$(BUILD)/spored-llama.o: examples/spored.c $(HDRS) | $(BUILD)
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) -Iexamples -Ibackends/llama \
		-DSPORE_WITH_LLAMA -c $< -o $@

$(BUILD)/spored-llama: $(BUILD)/spored-llama.o $(BUILD)/echo_backend.o \
		$(BUILD)/spore_llama.o $(BUILD)/libspore.a
	$(CXX) $(CFLAGS) $(LDFLAGS) $^ $(LLAMA_LIBS) $(LDLIBS) -ldl -fopenmp -o $@

llama: $(BUILD)/spored-llama

MODELS ?= ../cyllama/models
test-llama: $(BUILD)/spored-llama
	SPORED_LLAMA=$(BUILD)/spored-llama \
	SPORE_CHAT_MODEL=$(MODELS)/Qwen3-0.6B-Q8_0.gguf \
	SPORE_EMBED_MODEL=$(MODELS)/bge-small-en-v1.5-q8_0.gguf \
	uv run --no-project --with pytest --with openai pytest -q tests/test_llama.py

clean:
	rm -rf build build-asan build-tsan build-fuzz
