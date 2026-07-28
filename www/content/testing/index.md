---
title: How libiox is tested
date: 2026-07-27
abstract: Eighteen unit tests are the floor. Above them sit sanitizers, an allocation-failure sweep, static analysis, and a fuzzer that drives the API itself.
category: internals
tags: [testing, fuzzing, sanitizers]
---

libiox shipped four latent bugs across fourteen vendored copies for a year.
None were subtle. They survived because nobody ran the library in a
configuration other than the one it happened to be written for, and nobody
built it with the warnings turned up.

So the testing is built around that failure, not around a coverage target.

## The fast check

```sh
make run-tests
```

Eighteen unit tests, a fuzz corpus replay, and the allocation-failure sweep.
Seconds, and it is what you run while editing.

## The thorough check

```sh
tools/test.sh              # everything this machine can do
tools/test.sh --list       # name the stages
tools/test.sh sanitizers   # just one
```

A stage whose tool is missing is skipped and *reported* as skipped, so the
output always says what it did not cover rather than quietly passing.

| Stage | What it catches |
| --- | --- |
| `warnings` | gcc and clang at `-O0 -O1 -O2 -O3 -Os`, all `-Werror`. Optimisation level changes what the compiler can see. |
| `standards` | C99, C11, C17 |
| `sanitizers` | ASan, UBSan, clang's integer and implicit-conversion checks, and UBSan in trap mode |
| `hardening` | `_FORTIFY_SOURCE=3`, stack protector, stack clash, CET |
| `valgrind` | leaks and uninitialised reads, with `--errors-for-leak-kinds=all` |
| `m32` | 32-bit, where `long` and pointers stop agreeing |
| `analysis` | `gcc -fanalyzer` and `clang --analyze` |
| `oom` | every allocation failure path |
| `fuzz` | replays the committed corpus |
| `coverage` | gcov, per file |
| `mingw` | the Windows objects |

Sanitizer stages run with `-fno-sanitize-recover=all`, so the first report is
fatal. Without it a run can print a diagnostic and still exit zero, which is
the same as not running it.

Two are deliberately skipped and say so. MemorySanitizer needs every linked
library instrumented, including libc, so on a stock toolchain it reports the
C runtime instead of libiox. ThreadSanitizer has nothing to find: a loop and
everything on it belongs to one thread, by contract.

## Fuzzing the API, not the input

libiox has no parser and no wire format. Its input is the *order calls arrive
in*, which is exactly where its bugs were: removing a descriptor from inside
its own dispatch, cancelling a timer that already fired, freeing a loop that
still holds signals.

So `tests/iox_fuzz.c` treats each input byte as an opcode and the bytes after
it as arguments. A corpus becomes a corpus of call sequences.

```sh
tools/fuzz.sh          # 60 seconds
tools/fuzz.sh 900      # 15 minutes
```

Two details make it work. Every `poll` is preceded by a zero-millisecond
timer, because `iox_loop_poll` blocks until something is due and a fuzzer that
blocks has hung. And `SIGUSR1` and `SIGUSR2` are set to `SIG_IGN` before any
loop exists, so the harness can raise them freely: libiox saves that
disposition and restores it, which makes an unregistered raise a no-op rather
than a kill.

The harness also builds without libFuzzer, which is how CI replays the
committed corpus. A crash found once stays found.

## The allocation-failure sweep

`iox_loop_new` allocates four times: the loop, the timer state, the timer
heap, and the signal state. Each failure has its own unwind path, and none of
them run in a normal test. A leak there would sit undetected under valgrind
forever, because valgrind can only see paths that execute.

`tests/iox_oom.c` links with `-Wl,--wrap=calloc`, fails the Nth allocation,
and sweeps N across the whole range:

```
iox_loop_new makes 4 allocation(s)
  calloc #1 fails -> NULL
  calloc #2 fails -> NULL
  calloc #3 fails -> NULL
  calloc #4 fails -> NULL

iox_oom: all unwind paths clean
```

It learns the allocation count from a clean run rather than hard-coding it,
so adding an allocation extends the sweep automatically instead of silently
leaving the new path untested.

## Coverage, and what is not covered

```
iox_fd.c       97.6%
iox_signal.c   83.6%
iox_loop.c     81.3%
iox_timer.c    80.0%
iox_plat.c     27.8%
```

`iox_plat.c` is low *on a Linux run*, because half of it is the Windows
branch. That branch is no longer untested: `tests/iox_portable_test.c` runs
natively on Windows in CI under both mingw and MSVC, and under wine locally
via `tools/wine-test.sh`. The gap that used to be here, a Windows
implementation that had only ever been compiled, is closed.

The rest of the uncovered lines are allocation failures inside `poll`, which
the OOM sweep reaches for `iox_loop_new` but not yet elsewhere.

## Platforms

| Where | How | What runs |
| --- | --- | --- |
| Linux x86-64 | native | the full `tools/test.sh` matrix |
| Linux arm64 | native runner | build, tests, valgrind, sanitizers |
| Linux arm32 | cross + qemu | build and both test binaries |
| macOS arm64 | native | build, tests, ASan and UBSan |
| Windows x86-64 | native, mingw and MSVC | the portable test |
| Windows x86-64 | wine, locally | the portable test |

arm32 is the only target where `long` and pointers are 32-bit, which is
where a 64-bit nanosecond deadline calculation would go wrong if it were
written carelessly. It is worth keeping for that reason alone.

`iox_test.c` cannot build on Windows: it uses pipes, `sigaction`, `fcntl`,
and `SIGUSR1`, none of which exist there. `iox_portable_test.c` covers what
both platforms share, which is loopback UDP sockets, timers, `SIGINT` and
`SIGTERM`, and the multi-loop signal refcounting.

## Vendoring is tested too

The supported way to use libiox is to copy it, so a release that vendors
into something that does not build is a broken release. CI vendors the
commit under test with `tools/vendor.sh --from .`, then builds the result
the way `VENDORING.md` says a consumer would: one include path and five
sources, no modular-make. It also checks that globbing the vendored
directory for `*.c` yields five objects and no `main`, that the Windows
cross-compile still works from the vendored copy, and that `--force`
cleans up a previous layout instead of leaving stale files behind.

## One known false positive

`clang --analyze` reports a garbage read at the comparison in `pq.h`'s
`siftup_`, on the path where `resize()` reallocs the heap. It is wrong:
`siftup_` only ever reads `PQ__PARENT(i)`, which is strictly less than `i`
for every `i > 0`, so every read lands inside `[0, nr)` and is initialised.
The analyzer cannot show that `realloc` preserved the old entries.

`pq.h` is excluded from that one stage for that one reason, recorded in
`tools/test.sh`. Warnings in libiox's own files still fail it.
