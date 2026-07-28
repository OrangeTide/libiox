# libiox

A small `poll()`-based event loop for C. Descriptor watchers with
read/write event flags, one-shot timers on a binary heap, signal delivery
that runs your callback in normal context, and an idle callback between
iterations.

Six C files, no dependencies, no build step. POSIX and Windows.

```c
struct iox_loop *loop = iox_loop_new();

iox_fd_add(loop, fd, IOX_READ, on_readable, NULL);
iox_timer_add(loop, 1000, on_tick, NULL);
iox_signal_add(loop, SIGINT, on_signal, NULL);

iox_loop_run(loop);
iox_loop_free(loop);
```

## Documentation

- [Using libiox](https://orangetide.github.io/libiox/guide/) -- a working
  loop, and the things worth knowing about each piece
- [API reference](https://orangetide.github.io/libiox/api/) -- every
  function, what it returns, what it does on failure
- [How it works](https://orangetide.github.io/libiox/internals/) --
  dispatch, deferred removal, the signal split, the Windows port
- [Vendoring and versioning](VENDORING.md) -- taking a copy and upgrading it
- [How libiox is tested](https://orangetide.github.io/libiox/testing/) --
  sanitizers, fuzzing, and the allocation-failure sweep
- [Fourteen copies, one library](https://orangetide.github.io/libiox/consolidation/)
  -- where this came from, and the four bugs every copy shared
- [CHANGELOG.md](CHANGELOG.md)

## Getting it

libiox is copied into your tree rather than installed:

```sh
curl -fsSLO https://raw.githubusercontent.com/OrangeTide/libiox/v0.1.1/tools/vendor.sh
less vendor.sh
sh vendor.sh --version v0.1.1 --dest third_party/libiox
```

Then one include path and five sources. See [VENDORING.md](VENDORING.md).

## API

### Loop

- `iox_loop_new()` / `iox_loop_free()` -- create/destroy. Returns NULL
  if the loop, its timer heap, or its signal channel cannot be set up.
- `iox_loop_run()` -- run until `iox_loop_stop()` is called. Returns 0
  on a clean stop, -1 if `poll` failed for any reason other than
  `EINTR`.
- `iox_loop_poll(loop)` -- single poll iteration. Blocks until an fd
  is ready or the next timer expires. Returns number of events
  dispatched, -1 on error.
- `iox_loop_start(loop)` -- mark the loop running, for hand-rolled
  `iox_loop_poll` loops. `iox_loop_run` does this itself.
- `iox_loop_is_stopped(loop)` -- true once `iox_loop_stop` was called.
  (`iox_loop_stopped` is a deprecated alias, removed in 0.3.0.)
- `iox_loop_set_idle(loop, cb, arg)` -- called between iterations
  in `iox_loop_run`.

### File descriptors

- `iox_fd_add(loop, fd, events, cb, arg)` -- watch fd for
  `IOX_READ` and/or `IOX_WRITE`. The callback gets the flags that
  fired, plus `IOX_EXCEPT` on an error condition.
- `iox_fd_mod(loop, fd, events)` -- change watched events.
- `iox_fd_remove(loop, fd)` -- safe to call during dispatch.

Closing a watched fd without calling `iox_fd_remove` is a bug, but not
a fatal one. The loop reports the descriptor readable once, so the
callback's next read fails visibly, then drops it from the poll set.
It will not spin.

### Timers

- `iox_timer_add(loop, ms, cb, arg)` -- schedule a one-shot timer.
  Returns a timer ID (>= 0) on success.
- `iox_timer_remove(loop, id)` -- cancel a pending timer.

Timers are one-shot: they fire once and are automatically removed.
Re-add from the callback for repeating behavior. Backed by a min-heap
priority queue keyed on monotonic deadlines.

### Signals

- `iox_signal_add(loop, signo, cb, arg)` -- register handler.
  Re-registering a signal already on this loop replaces its callback.
- `iox_signal_remove(loop, signo)` -- unregister on this loop.

Signal callbacks run in normal context, not in the signal handler, so
they may call any function.

Callbacks are per-loop, but the signal *disposition* belongs to the
process. Several loops may watch the same signal and each gets its own
callback; the previous disposition is restored only once the last loop
watching it drops it.

On POSIX, delivery uses the classic self-pipe: the handler writes the
signal number to a non-blocking pipe the loop already watches. On
Windows there is no equivalent, because anonymous pipes are not pollable
by `WSAPoll` and the CRT runs the `SIGINT` handler on its own thread.
The Windows build instead binds a UDP loopback socket and has the
handler poke it with a one-byte datagram carrying the signal number.

## Limits

Fixed capacities, no dynamic growth for fds:

- `IOX_MAX_FDS` 64 watched descriptors per loop
- `IOX_MAX_SIGNALS` 8 registered signals per loop, and 8 distinct
  signals across the process
- `IOX_MAX_TIMERS` 16 initial timers per loop, the heap grows on demand

All fd, timer, and signal state is per-loop. Multiple loops in one
process are supported and do not interfere with each other. The library
is not thread-safe; a loop and everything on it belongs to one thread.

Timer IDs are non-negative and unique among *pending* timers. They are
recycled once a timer fires or is removed, and the counter wraps at
`INT_MAX` rather than overflowing.

## Building

Uses modular-make. The library target is `iox`:

    make                # build
    make run-tests      # build and run the test suite
    make run-tests-valgrind

Consumers vendor `src/` and add to their own `module.mk`:

    iox_LIBS = iox

`iox_EXPORTED_CPPFLAGS` carries the include path, and on a mingw target
`iox_EXPORTED_LDLIBS` adds `-lws2_32`.

## Testing

    make run-tests        # the fast check: unit tests, fuzz replay, OOM sweep
    tools/test.sh         # the thorough one
    tools/fuzz.sh 600     # fuzz the API for ten minutes
    tools/wine-test.sh    # run the Windows build under wine

`tools/test.sh` builds and runs the suite under both compilers at every
optimisation level, three C standards, ASan, UBSan, valgrind, a hardened
build, 32-bit, `gcc -fanalyzer`, and `clang --analyze`, then sweeps every
allocation-failure path and replays the fuzz corpus. Missing tools are
skipped and reported as skipped, never passed over silently.

CI additionally builds and runs the suite on Linux arm64 natively, arm32
under qemu, macOS, and Windows x86-64 with both mingw and MSVC, and checks
that `tools/vendor.sh` produces a copy that builds.

See [How libiox is tested](https://orangetide.github.io/libiox/testing/)
for what each stage catches and what is still uncovered.

## Repository layout

| Path | What |
| --- | --- |
| `src/` | the library, and nothing else: `cc src/*.c` builds it |
| `tests/` | test suite, fuzz harness, allocation-failure sweep |
| `tools/version.sh` | read, check, and bump the version; create the tag |
| `tools/vendor.sh` | copy a release into another project's tree |
| `tools/test.sh` | the full test matrix |
| `tools/fuzz.sh` | libFuzzer driver for the API fuzz target |
| `tools/wine-test.sh` | run the Windows build under wine |
| `www/` | the documentation site, built with make and lowdown |
| `docs/coding-style.md` | the style all C in this tree follows |
| `.github/workflows/ci.yml` | Linux, arm64, arm32, macOS, Windows, vendoring |
| `.github/workflows/pages.yml` | build the site and deploy it |

`www/` is separate because GNU make prefers `GNUmakefile` over `Makefile`,
so a site `Makefile` at the root would be shadowed by the C build. Build
the site with `make -C www` and preview it with `make -C www serve`.

## Files

| File | Role |
| --- | --- |
| `iox_loop.c` / `.h` | event loop, poll and dispatch |
| `iox_fd.c` / `.h` | descriptor watcher table |
| `iox_timer.c` / `.h` | one-shot timers |
| `iox_signal.c` / `.h` | signal delivery |
| `iox_internal.h` | `struct iox_loop`, shared between loop and fd |
| `iox_plat.c` / `.h` | poll, socket, and monotonic clock shim |
| `iox_version.h` | what version this copy is |
| `pq.h` | single-header binary heap used by the timer queue |

`iox_internal.h` and `iox_plat.h` are implementation detail. Consumers
include only the four public headers.

`pq.h` is vendored. It has no central home; it is a file that gets
copied between projects, so this copy is authoritative for libiox and
copies elsewhere may differ.

## License

Copyright (c) 2026 Jon Mayo.

Dual-licensed: `MIT-0 OR CC0-1.0`. Take whichever you prefer. Neither
requires attribution. See `LICENSE`.
