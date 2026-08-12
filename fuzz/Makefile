# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0

FUZZ_CC ?= clang

FUZZ_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT_DIR := $(abspath $(FUZZ_DIR)/..)
BUILD_DIR := $(ROOT_DIR)/build/fuzz
BIN_DIR := $(BUILD_DIR)/bin
CORPUS_DIR := $(BUILD_DIR)/corpus
ARTIFACT_DIR := $(BUILD_DIR)/artifacts
ROM_CORPUS_DIR := $(CORPUS_DIR)/rom
YAZ0_CORPUS_DIR := $(CORPUS_DIR)/yaz0
ROM_ARTIFACT_DIR := $(ARTIFACT_DIR)/rom
YAZ0_ARTIFACT_DIR := $(ARTIFACT_DIR)/yaz0

ROM_FUZZER := $(BIN_DIR)/rom_util_fuzz
YAZ0_FUZZER := $(BIN_DIR)/yaz0_fuzz
FUZZERS := $(ROM_FUZZER) $(YAZ0_FUZZER)
DICTIONARY := $(FUZZ_DIR)/rom_util.dict
ROM_UTIL_SOURCE := $(ROOT_DIR)/src/rom_util.c
ROM_UTIL_HEADERS := $(ROOT_DIR)/src/rom_util.h \
	$(ROOT_DIR)/src/liboot.h $(ROOT_DIR)/src/liboot_engine.h

FUZZ_FLAGS := -std=c11 -g -O1 -fno-omit-frame-pointer \
	-fsanitize=fuzzer,address,undefined
FUZZ_CPPFLAGS ?=
FUZZ_CFLAGS ?=
FUZZ_LDFLAGS ?=
FUZZ_LDLIBS ?=
FUZZ_MAX_LEN ?= 4194304
FUZZ_TIMEOUT ?= 10
FUZZ_RSS_LIMIT_MB ?= 2048
SMOKE_RUNS ?= 1000
FUZZ_UBSAN_OPTIONS ?= halt_on_error=1:print_stacktrace=1

HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
ifeq ($(HOST_OS),Darwin)
# Apple's process/runtime initialization leaves allocations visible to LSan.
# ASan and UBSan remain enabled; only leak reporting is disabled for fuzz runs.
FUZZ_ASAN_OPTIONS ?= detect_leaks=0:halt_on_error=1
else
FUZZ_ASAN_OPTIONS ?= detect_leaks=1:halt_on_error=1
endif

all: $(FUZZERS)

$(ROM_FUZZER): $(FUZZ_DIR)/rom_util_fuzz.c $(ROM_UTIL_SOURCE) $(FUZZ_DIR)/Makefile \
		$(ROM_UTIL_HEADERS) | $(BIN_DIR)
	$(FUZZ_CC) $(FUZZ_CPPFLAGS) $(FUZZ_CFLAGS) $(FUZZ_FLAGS) -I$(ROOT_DIR)/src \
		$(FUZZ_DIR)/rom_util_fuzz.c $(ROM_UTIL_SOURCE) \
		$(FUZZ_LDFLAGS) $(FUZZ_LDLIBS) -o $@

$(YAZ0_FUZZER): $(FUZZ_DIR)/yaz0_fuzz.c $(ROM_UTIL_SOURCE) $(FUZZ_DIR)/Makefile \
		$(ROOT_DIR)/src/rom_util.h | $(BIN_DIR)
	$(FUZZ_CC) $(FUZZ_CPPFLAGS) $(FUZZ_CFLAGS) $(FUZZ_FLAGS) -I$(ROOT_DIR)/src \
		$(FUZZ_DIR)/yaz0_fuzz.c $(ROM_UTIL_SOURCE) \
		$(FUZZ_LDFLAGS) $(FUZZ_LDLIBS) -o $@

$(BIN_DIR) $(ROM_CORPUS_DIR) $(YAZ0_CORPUS_DIR) \
		$(ROM_ARTIFACT_DIR) $(YAZ0_ARTIFACT_DIR):
	mkdir -p "$@"

smoke: all $(DICTIONARY) | $(ROM_CORPUS_DIR) $(YAZ0_CORPUS_DIR) \
		$(ROM_ARTIFACT_DIR) $(YAZ0_ARTIFACT_DIR)
	ASAN_OPTIONS="$(FUZZ_ASAN_OPTIONS)" UBSAN_OPTIONS="$(FUZZ_UBSAN_OPTIONS)" \
		$(ROM_FUZZER) -runs=$(SMOKE_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-dict=$(DICTIONARY) -artifact_prefix=$(ROM_ARTIFACT_DIR)/ \
		$(ROM_CORPUS_DIR)
	ASAN_OPTIONS="$(FUZZ_ASAN_OPTIONS)" UBSAN_OPTIONS="$(FUZZ_UBSAN_OPTIONS)" \
		$(YAZ0_FUZZER) -runs=$(SMOKE_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-dict=$(DICTIONARY) -artifact_prefix=$(YAZ0_ARTIFACT_DIR)/ \
		$(YAZ0_CORPUS_DIR)

clean:
	rm -rf "$(BUILD_DIR)"

.PHONY: all smoke clean
