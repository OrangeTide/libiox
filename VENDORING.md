# Vendoring libiox

libiox is built to be copied into another project's tree rather than
installed. There is no shared library to ship, no `pkg-config` file, and no
version to negotiate at run time. You take the source, you own the copy, and
you upgrade when you decide to.

This is not an aspiration. libiox spent its first year as fourteen divergent
copies precisely because it was easy to copy and had no release mechanism.
The scripts here are that missing mechanism.

## Getting a release

Releases are git tags of the form `vMAJOR.MINOR.PATCH`, and GitHub serves a
source snapshot for every tag at a fixed URL:

```
https://github.com/OrangeTide/libiox/archive/refs/tags/<tag>.tar.gz
```

`tools/vendor.sh` fetches exactly that, copies the library, and writes a
`VENDORED.md` recording the version so the next upgrade knows where it
started. With no `--version` it takes the latest release:

```sh
curl -fsSL https://raw.githubusercontent.com/OrangeTide/libiox/main/tools/vendor.sh \
  | sh -s -- --dest third_party/libiox
```

For a repeatable build, name the release and take the script from that same
tag rather than from `main`. A tag names an immutable commit, so the script
you run is the one that shipped with the release and cannot change under you.
Piping anything from the network into a shell means trusting the server and
the connection; reading it first is the better habit:

```sh
TAG=v0.1.1
curl -fsSLO "https://raw.githubusercontent.com/OrangeTide/libiox/$TAG/tools/vendor.sh"
less vendor.sh && sh vendor.sh --version "$TAG"
```

`IOX_VENDOR_REPO`, `IOX_VENDOR_BASE_URL`, and `IOX_VENDOR_API_URL` override
where it fetches from, for a fork, a mirror, or an air-gapped copy.

None of this needs the script. libiox is one directory of C files, so a
`tar -xzf` and a `cp` does the same job.

## What to copy

There are no optional layers. The library is thirteen files and you take all
of them:

| Files | What |
| --- | --- |
| `iox_loop.c` `.h` | the event loop |
| `iox_fd.c` `.h` | descriptor watchers |
| `iox_timer.c` `.h` | one-shot timers |
| `iox_signal.c` `.h` | signal delivery |
| `iox_plat.c` `.h` | poll, socket, and clock shim |
| `iox_internal.h` | `struct iox_loop` |
| `iox_version.h` | what version this copy is |
| `pq.h` | the binary heap the timers use |

Tests are not part of the library and live in `tests/` upstream. `--with-tests`
copies them into `<dest>/tests/`, deliberately not alongside the sources, so a
host build that globs the vendored directory for `*.c` gets the library and no
stray `main()`. Taking them and running them after each upgrade is the better
habit.

`--no-module-mk` skips the library's `module.mk`, which is otherwise copied
because it declares the library alone and is inert to a project that does not
use modular-make. Use it when your project keeps its own wiring for libiox,
which is the common case once target names or platform guards differ from the
ones upstream uses. `--force` will not touch a `module.mk` it is not going to
replace, so the file is yours to keep.

## Fitting it to your build

Every file includes its own headers by plain name, so the only thing a host
build system supplies is one `-I`:

```make
IOX_SRCS     = third_party/libiox/iox_loop.c \
               third_party/libiox/iox_fd.c \
               third_party/libiox/iox_signal.c \
               third_party/libiox/iox_timer.c \
               third_party/libiox/iox_plat.c
IOX_CPPFLAGS = -Ithird_party/libiox
```

On Windows add `-lws2_32`. There is no configure step. The platform
differences that exist are selected by the compiler's own `_WIN32`, so
nothing has to be detected.

If your project also uses modular-make, `src/module.mk` in this repository
shows the shape, but it declares the test binary too, so copy the four lines
you want rather than the file.

## Identifying a copy

A vendored copy has no git and no tags, so `iox_version.h` is the only thing
that can say what it is:

```c
#include "iox_version.h"

#if IOX_VERSION < 101          /* 0.1.1 */
#  error "libiox 0.1.1 or newer is required"
#endif
```

`IOX_VERSION_STRING` is the same value as text.

`src/iox_version.h` is the single source of truth. The git tag mirrors it,
never the other way round, and so does every version quoted in this
documentation: `tools/version.sh --sync` rewrites them from the header, and
`--bump` and `--set` run it for you. `--check` fails if anything drifted,
and CI runs `--check` on every push, so a release cannot ship documentation
pointing at the previous one.

Examples that are illustrative rather than current use `v1.2.3`, which is
obviously not a real release and so never needs updating.

## Upgrading

Re-running `tools/vendor.sh --force` with a newer `--version` is the whole
upgrade. Then read [CHANGELOG.md](CHANGELOG.md) and run the tests.

Do not edit a vendored copy in place. Local changes are lost on the next
upgrade and make it impossible to tell what version you are on. If you need
a change, send it upstream or keep it as a patch you re-apply, and say so in
`VENDORED.md`.

While the major version is 0, a minor bump may break the API. The CHANGELOG
records every break with the reason for it.

## Third-party code in this repository

### pq.h

- **What it is:** a single-header binary heap, used for the timer queue.
- **Upstream:** none. `pq.h` is a file that gets copied between projects
  rather than tracked anywhere central. Copies exist in several trees and
  they differ. **This copy is authoritative for libiox.**
- **Local changes:** `PQ__API` is `static __attribute__((unused))` on GCC and
  Clang, so a translation unit that uses part of the API does not warn about
  the rest. Copies elsewhere use a `#pragma GCC diagnostic` at the include
  site instead, or nothing at all.
- **Licence:** MIT-0 OR CC0-1.0, the same as libiox.

### modular-make (`GNUmakefile`)

- **Version:** 1.8.7
- **What it is:** a modular multi-target build driver. The `module.mk` files
  in this tree are its input.
- **Why vendored:** so a bare checkout builds with nothing but GNU make and a
  C compiler.
- **Local changes:** none.

## Licence

`MIT-0 OR CC0-1.0`. Copy it, change it, ship it, relicense it. No
attribution required either way.
