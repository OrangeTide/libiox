# Changelog

Versions are SemVer. The major version is the compatibility promise for the
API. While the major version is 0, a minor bump may break the API.

## 0.1.1

A bug fix in `tools/vendor.sh`, two CI corrections, and a set of notes on
sending UDP fast. The library itself is unchanged, so a copy already on
0.1.0 does not need to be re-vendored for this.

### Added

- **`docs/udp-perf/`, notes and a measurement program for high-rate UDP
  sending on Linux.** `udpzc.c` sends the same datagram stream four ways so
  they compare directly: `sendmsg`, `sendmsg` with `MSG_ZEROCOPY`,
  `IORING_OP_SENDMSG`, and `IORING_OP_SENDMSG_ZC`, with UDP GSO optional on
  each. The io_uring path calls `io_uring_setup` and `io_uring_enter`
  directly and maps the rings itself, so there is no liburing dependency.
  It is Linux only and is not wired into `module.mk`, so the portable build
  is untouched.

  Batching is what pays. UDP GSO is worth roughly 6x on its own and
  io_uring another 5 to 7 percent on top; zero-copy is a second order
  effect that only appears above about 10 KB per send, and both zero-copy
  paths report when the kernel copied anyway so you can tell.

  The notes also record where a `poll()` loop stops helping, which is the
  part that concerns libiox. GSO needs nothing from the event loop.
  `MSG_ZEROCOPY` fits, because `poll()` reports `POLLERR` whether or not it
  was asked for. io_uring does not fit: registering the ring fd as a
  watcher works, but pays a `poll()` wakeup per batch for a mechanism whose
  point is avoiding syscalls. A program that needs io_uring wants io_uring
  owning its event loop.

- The same material as a page on the site, written for a reader who does
  not use libiox, since most of it is about Linux rather than about this
  library.

### Fixed

- **`tools/vendor.sh --force` no longer deletes a `module.mk` it will not
  replace.** With `--no-module-mk` the `module.mk` in the destination is the
  host project's own build wiring, which the script did not put there. The
  `--force` cleanup removed it anyway and then wrote nothing back, silently
  destroying it.

  This matters because most projects vendoring libiox do keep their own
  wiring: target names and platform guards differ between them, so
  `--no-module-mk` is the normal way to upgrade, not an unusual one. The bug
  cost one project its `module.mk` on the first real re-vendor.

  The cleanup now clears `module.mk` only when the run is going to write one.
  CI covers both directions: a host-owned file survives `--force`, and one
  the run does own is still replaced rather than left stale.

- **The arm32 CI job installs the target libc explicitly.** It failed
  compiling `src/iox_fd.c` on a missing `bits/libc-header-start.h`. The
  cross compiler had no target headers, so gcc's own `stdint.h` fell
  through `include_next` to the runner's x86 headers.
  `libc6-dev-armhf-cross` reaches the compiler only as a Recommends, and
  the job installs with `--no-install-recommends`. A preflight step now
  compiles `stdint.h` and `poll.h` with the cross compiler and names the
  missing package. A bare `main()` probe would not catch it, since a
  toolchain with no target libc compiles one fine.

  Worth knowing on its own: this passed locally because a non-multiarch
  machine has `/usr/include/bits/libc-header-start.h`, so the fallback to
  host headers succeeds silently and produces a binary compiled against the
  wrong libc headers.

### Changed

- The Pages workflow moved to the Node 24 action releases.
  `upload-pages-artifact@v5` and `deploy-pages@v5` clear the runner's
  forced-Node-24 annotation that reached it transitively through
  `upload-artifact`. `pages.yml` also picked up `checkout@v6` to match
  `ci.yml`.

## 0.1.0

First release of the consolidated library. libiox had been vendored and
forked into fourteen trees; this is the merge point.

### Added

- Windows support, taken from the newest fork and made self-contained. That
  fork reached Windows through its host project's platform library; the
  three functions libiox used are now `iox_plat.c` / `.h`, so the library
  still depends on nothing.
- `iox_version.h`, so a vendored copy can identify itself at compile time
  with `IOX_VERSION`.
- `tools/vendor.sh` to copy a release into another tree, and
  `tools/version.sh` to read, check, and bump the version.
- Test coverage for multiple loops, invalid descriptors, and timer id reuse.
- `tests/iox_fuzz.c`, a fuzz target that drives the API itself: each input
  byte is an opcode, so a corpus is a corpus of call sequences. Runs under
  libFuzzer via `tools/fuzz.sh`, and standalone for corpus replay.
- `tests/iox_oom.c`, which fails each of the four allocations in
  `iox_loop_new` in turn and checks every unwind path. Those paths never
  execute in a normal run, so valgrind could not see them.
- `tools/test.sh`, the full matrix: two compilers at five optimisation
  levels, three C standards, ASan, UBSan, UBSan trap mode, clang's integer
  and implicit-conversion checks, a hardened build, valgrind, 32-bit,
  `gcc -fanalyzer`, `clang --analyze`, coverage, and the Windows objects.
  Missing tools are reported as skipped rather than passed over.
- `tests/iox_portable_test.c`, the subset that builds on Windows too:
  loopback UDP sockets, timers, and `SIGINT`/`SIGTERM`. `iox_test.c` is
  POSIX and cannot run there, so before this the Windows half of the
  library had only ever been compiled, never executed.
- `tools/wine-test.sh`, which cross-compiles and runs that under wine,
  and CI jobs that run it natively on Windows with both mingw and MSVC.
- `.github/workflows/ci.yml`: Linux x86-64 (full matrix), Linux arm64
  natively, arm32 under qemu, macOS, Windows x86-64 with two toolchains,
  and a job that vendors the commit under test and builds the result the
  way a consumer would.
- `tools/version.sh --sync`, so `src/iox_version.h` is the only place a
  version is written by hand. It rewrites the version in the documented
  install commands; `--bump` and `--set` run it automatically and
  `--check` fails on drift.

### Fixed

- **Timer and signal state is per-loop.** All of it had been file-scope, and
  every `loop` argument in `iox_timer.c` and `iox_signal.c` was ignored. A
  second loop never received signals at all, and freeing any loop restored
  `SIG_DFL` for handlers belonging to other loops, killing the process on the
  next signal. Signal dispositions are process-wide by nature, so those now
  carry a refcount and the last loop out restores the previous handler.
- **`POLLNVAL` no longer spins the loop.** A descriptor closed without
  `iox_fd_remove` produced no event and no dispatch, so `poll` returned
  instantly forever at 100% CPU. It is now reported readable once, then
  dropped from the poll set.
- **`iox_loop_run` checks `errno`.** It continued on any `poll` failure, so
  a persistent error span the same way. It now continues only on `EINTR` and
  returns -1 otherwise.
- **Timer ids no longer overflow.** `next_id` was a plain `int` incremented
  forever; signed overflow is undefined, and negative ids collide with the
  -1 error return. Ids now wrap at `INT_MAX` and skip values a pending timer
  still holds.
- `iox_loop_new` reports failure instead of returning a loop whose timer or
  signal support silently never started. It can now return `NULL` where it
  previously could not.
- `iox_fd_*` comparisons cast through `iox_pollfd_t`, since `struct
  pollfd::fd` is an unsigned `SOCKET` on Windows. Warning-only, but every
  fork had it.

### Changed

- `iox_loop_stopped` is now `iox_loop_is_stopped` and returns `bool`. It
  answers a question rather than reporting a status, and the old name read
  as the latter. `iox_loop_stopped` remains as a macro alias, since the
  pre-consolidation forks call it from four places; it is removed in 0.2.0.
  Define `IOX_NO_DEPRECATED` to drop the alias and prove a copy has
  finished migrating.
- `iox_signal_add` on a signal already registered on the same loop replaces
  the callback. It used to append a second entry, so both fired and
  `iox_signal_remove` restored a confused handler chain.
- `iox_fd_add`, `iox_fd_mod`, and `iox_fd_remove` moved from `iox_loop.c`
  into `iox_fd.c`, which had been a comment-only file and therefore an empty
  translation unit that `-Wpedantic` rejects. `struct iox_loop` moved to
  `iox_internal.h`.
- Relicensed to `MIT-0 OR CC0-1.0`. Previously "MIT-0 OR PUBLIC DOMAIN",
  which named no specific public domain instrument.
- Adopted the shared coding style, now carried at `docs/coding-style.md`.
  Four spaces instead of hard tabs, no per-file licence block (the root
  `LICENSE` is the one place), `IOX_OK` and `IOX_ERR` instead of bare `0`
  and `-1`, and `test_iox.c` renamed to `iox_test.c`. `pq.h` is vendored
  and exempt. Behaviour is unchanged; the sweep was done in one commit
  before the first tag, while there were no downstream copies to disturb.
- Tests moved out of `src/` into `tests/`, so `cc src/*.c` builds the
  library and only the library. A vendored copy takes tests into
  `<dest>/tests/` with `--with-tests`, or not at all.
