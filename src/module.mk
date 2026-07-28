# src/module.mk -- libiox: poll()-based I/O multiplexer
#
# This directory is the library and nothing else, so a project that would
# rather not adopt modular-make can compile src/*.c directly and be done.
# Tests and the fuzz harness live in tests/ for that reason.
#
# Skip on emscripten; libiox uses poll/signals/self-pipe which don't
# make sense in a WebAssembly single-threaded main loop.
ifeq ($(findstring emscripten,$(TARGET_TRIPLET)),)

LIBRARIES += iox
iox_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
iox_SRCS = iox_loop.c iox_fd.c iox_signal.c iox_timer.c iox_plat.c
iox_EXPORTED_CPPFLAGS = -I$(iox_DIR)

# Windows: WSAPoll, WSAStartup, and the loopback self-socket live in winsock2.
ifneq ($(filter mingw% %-mingw32 %windows%,$(TARGET_TRIPLET)),)
iox_EXPORTED_LDLIBS = -lws2_32
endif

endif
