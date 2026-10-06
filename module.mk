# module.mk -- top level
#
# src/ is the library alone. tests/ holds the test suite and the fuzz
# harness, so that a project vendoring libiox can compile src/*.c without
# sweeping up a main() it does not want.

SUBDIRS = src tests

# --- Developer check targets ------------------------------------------
#
# Shortcuts a developer and CI reach for. The library and test suite are
# defined declaratively in src/module.mk and tests/module.mk; this section
# is only plain wrappers around the test tooling, built the way a vendoring
# consumer would (one -I and the five sources), kept separate from the fast
# `make run-tests`.
#
#   make asan     build and run the suite under AddressSanitizer
#   make ubsan    build and run the suite under UndefinedBehaviorSanitizer
#   make gcov     line coverage of the library from the suite
#   make torture  the full aggressive matrix (tools/test.sh)
#   make fuzz     libFuzzer run, default 60s: make fuzz FUZZ_SECS=600

IOX_SRCS    := $(wildcard src/iox_*.c)
IOX_SUITE   := tests/iox_test.c
IOX_CFLAGS  := -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
IOX_SANBASE := -O1 -g -fno-sanitize-recover=all
IOX_CHECK   := _build/check
FUZZ_SECS   ?= 60

.PHONY : asan
asan :
	@mkdir -p $(IOX_CHECK)
	$(CC) $(IOX_CFLAGS) $(IOX_SANBASE) -fsanitize=address -Isrc \
	    -o $(IOX_CHECK)/iox_test_asan $(IOX_SUITE) $(IOX_SRCS)
	$(IOX_CHECK)/iox_test_asan

.PHONY : ubsan
ubsan :
	@mkdir -p $(IOX_CHECK)
	$(CC) $(IOX_CFLAGS) $(IOX_SANBASE) -fsanitize=undefined -Isrc \
	    -o $(IOX_CHECK)/iox_test_ubsan $(IOX_SUITE) $(IOX_SRCS)
	$(IOX_CHECK)/iox_test_ubsan

.PHONY : gcov
gcov :
	tools/test.sh coverage

.PHONY : torture
torture :
	tools/test.sh

.PHONY : fuzz
fuzz :
	tools/fuzz.sh $(FUZZ_SECS)
