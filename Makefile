# Frontend to CMake. Build logic lives in CMakeLists.txt; this file only
# maps short targets onto cmake/ctest invocations.

BUILD   ?= build
MODULES ?= ws llm realtime
CMAKE   ?= cmake
CTEST   ?= ctest
JOBS    ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

KNOWN_MODULES = ws llm realtime
$(foreach m,$(MODULES),$(if $(filter $(m),$(KNOWN_MODULES)),,\
  $(error unknown module '$(m)'; known: $(KNOWN_MODULES))))
$(if $(and $(filter realtime,$(MODULES)),$(if $(filter ws,$(MODULES)),,x)),\
  $(error module 'realtime' needs 'ws'))
onoff   = $(if $(filter $(1),$(MODULES)),ON,OFF)
CONFIG  = -DSPORE_WS=$(call onoff,ws) -DSPORE_LLM=$(call onoff,llm) \
          -DSPORE_REALTIME=$(call onoff,realtime) $(CMAKE_ARGS)

# Optional engine adapters (make engines / test-engines).
LLAMA_DIR   ?= ../cyllama/thirdparty/llama.cpp
WHISPER_DIR ?= ../cyllama/thirdparty/whisper.cpp
MODELS      ?= $(HOME)/.models
ASR_SAMPLE  ?= ../cyllama/build/whisper.cpp/samples/jfk.wav

FUZZ_CC   ?= clang
FUZZ_TIME ?= 60
FUZZ      = build-fuzz

.PHONY: all configure test unit replay integration asan tsan check-modules \
        fuzz fuzz-run autobahn engines test-engines install clean

all: configure
	$(CMAKE) --build $(BUILD) -j$(JOBS)

configure:
	@$(CMAKE) -S . -B $(BUILD) $(CONFIG) > /dev/null

test: all
	$(CTEST_PREFIX) $(CTEST) --test-dir $(BUILD) --output-on-failure -LE engines

unit: all
	$(CTEST) --test-dir $(BUILD) --output-on-failure -R '^unit$$'

replay: all
	$(CTEST) --test-dir $(BUILD) --output-on-failure -R '^replay_'

integration: all
	$(CTEST) --test-dir $(BUILD) --output-on-failure -R '^integration$$'

asan:
	$(MAKE) BUILD=build-asan \
		CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug -DSPORE_SANITIZE=address,undefined" test

# setarch -R: TSan cannot map its shadow memory under high-entropy ASLR.
tsan:
	$(MAKE) BUILD=build-tsan CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug -DSPORE_SANITIZE=thread" \
		CTEST_PREFIX="$(if $(shell command -v setarch),setarch $(shell uname -m) -R)" test

# Build and test each module set in its own directory.
MODULE_SETS = core ws llm ws+llm ws+realtime ws+llm+realtime
check-modules:
	@for set in $(MODULE_SETS); do \
		mods=$$(echo $$set | sed 's/core//; s/+/ /g'); \
		echo "== MODULES='$$mods'"; \
		$(MAKE) --no-print-directory BUILD=build-mod-$$set MODULES="$$mods" test || exit 1; \
	done

# ---- libFuzzer (clang) -------------------------------------------------------
# New corpus entries go to build-fuzz/corpus-*; copy crash reproducers into
# tests/fuzz/seeds/ so `make test` replays them. FUZZ_TIME is per target.

fuzz:
	@$(CMAKE) -S . -B $(FUZZ) -DCMAKE_C_COMPILER=$(FUZZ_CC) -DSPORE_FUZZ=ON \
		-DSPORE_BUILD_TESTS=OFF -DSPORE_BUILD_EXAMPLES=OFF > /dev/null
	$(CMAKE) --build $(FUZZ) -j$(JOBS) --target fuzz_http fuzz_json fuzz_ws fuzz_rt fuzz_conn

fuzz-run: fuzz
	for t in http json ws rt conn; do \
		mkdir -p $(FUZZ)/corpus-$$t; \
		$(FUZZ)/fuzz_$$t -max_total_time=$(FUZZ_TIME) -dict=tests/fuzz/$$t.dict \
			-artifact_prefix=$(FUZZ)/ $(FUZZ)/corpus-$$t tests/fuzz/seeds/$$t || exit 1; \
	done

# ---- Autobahn WebSocket testsuite (needs Docker) -----------------------------
# The fuzzing client connects to spored's /ws/echo over the host network.
# Reports go to $(BUILD)/autobahn/index.html.

autobahn: all
	python3 tests/autobahn/run.py $(BUILD)/spored $(BUILD)/autobahn

# ---- engine adapters -------------------------------------------------------
# spored-engines: llama.cpp (LLM) and whisper.cpp (ASR) behind the mock TTS.

engines:
	@$(CMAKE) -S . -B $(BUILD) $(CONFIG) -DSPORE_LLAMA_DIR=$(abspath $(LLAMA_DIR)) \
		-DSPORE_WHISPER_DIR=$(abspath $(WHISPER_DIR)) -DSPORE_TEST_MODELS=$(abspath $(MODELS)) \
		-DSPORE_ASR_SAMPLE=$(abspath $(ASR_SAMPLE)) > /dev/null
	$(CMAKE) --build $(BUILD) -j$(JOBS)

test-engines: engines
	$(CTEST) --test-dir $(BUILD) --output-on-failure -L engines

# ---- install / clean -----------------------------------------------------------

PREFIX ?= /usr/local
install: all
	$(CMAKE) --install $(BUILD) --prefix $(PREFIX)

clean:
	rm -rf build build-asan build-tsan build-fuzz build-mod-*
