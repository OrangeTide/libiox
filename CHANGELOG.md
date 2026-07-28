# Changelog

Versions are SemVer. The major version is the compatibility promise for the
API. While the major version is 0, a minor bump may break the API.

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
