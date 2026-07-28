# tests/module.mk -- the test suite and the fuzz harness
#
# Kept out of src/ so that compiling src/*.c builds the library and only the
# library. Nothing here is part of a vendored copy unless asked for.

ifeq ($(findstring emscripten,$(TARGET_TRIPLET)),)

EXECUTABLES += iox_test
iox_test_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
iox_test_SRCS = iox_test.c
iox_test_LIBS = iox

define iox_test_TESTCMD
$(iox_test_RUN)
endef
TEST_TARGETS += iox_test

# The portable subset: sockets, timers, and SIGINT/SIGTERM only, so it builds
# and runs on Windows too. iox_test.c above is POSIX and cannot. This is what
# gives the Windows half of the library any runtime coverage at all, natively
# in CI and under wine locally (tools/wine-test.sh).
EXECUTABLES += iox_portable_test
iox_portable_test_DIR := $(iox_test_DIR)
iox_portable_test_SRCS = iox_portable_test.c
iox_portable_test_LIBS = iox

define iox_portable_test_TESTCMD
$(iox_portable_test_RUN)
endef
TEST_TARGETS += iox_portable_test

# The standalone build of the fuzz harness: no libFuzzer needed, replays a
# corpus and runs a deterministic sweep. Fuzzing proper is tools/fuzz.sh.
EXECUTABLES += iox_fuzz
iox_fuzz_DIR := $(iox_test_DIR)
iox_fuzz_SRCS = iox_fuzz.c
iox_fuzz_LIBS = iox
iox_fuzz_CPPFLAGS = -DIOX_FUZZ_STANDALONE

define iox_fuzz_TESTCMD
$(iox_fuzz_RUN) $(wildcard $(iox_fuzz_DIR)fuzz-corpus/*)
endef
TEST_TARGETS += iox_fuzz

# Allocation-failure sweep. --wrap is a GNU ld feature; on a linker without
# it, drop this target rather than the whole test run.
EXECUTABLES += iox_oom
iox_oom_DIR := $(iox_test_DIR)
iox_oom_SRCS = iox_oom.c
iox_oom_LIBS = iox
iox_oom_LDFLAGS = -Wl,--wrap=calloc

define iox_oom_TESTCMD
$(iox_oom_RUN)
endef
TEST_TARGETS += iox_oom

endif
