---
title: API reference
date: 2026-07-26
abstract: Every function libiox exports, what it returns, and what it does on failure. Four public headers, twenty functions.
category: reference
tags: [c, api]
---

Four public headers. `iox_internal.h` and `iox_plat.h` are implementation
detail and consumers do not include them.

## iox_loop.h

```c
struct iox_loop *iox_loop_new(void);
void iox_loop_free(struct iox_loop *loop);

int  iox_loop_poll(struct iox_loop *loop);
int  iox_loop_run(struct iox_loop *loop);
void iox_loop_stop(struct iox_loop *loop);
void iox_loop_start(struct iox_loop *loop);
bool iox_loop_is_stopped(const struct iox_loop *loop);

void iox_loop_set_idle(struct iox_loop *loop, iox_idle_cb cb, void *arg);
```

`iox_loop_new` returns `NULL` if the loop, its timer heap, or its signal
channel cannot be set up. It allocates three times: the loop, the timer
state, and the signal state.

`iox_loop_poll` blocks until a descriptor is ready or the next timer is due.
It returns the number of descriptor callbacks it dispatched, or -1 with
`errno` set from `poll`. Expired timers are dispatched at the end of the same
call but are not counted in the return value. With no descriptors and no
timers it blocks forever.

`iox_loop_run` polls until `iox_loop_stop`, calling the idle callback between
iterations. It returns 0 on a clean stop and -1 if `poll` failed for any
reason other than `EINTR`. It does not return -1 on `EINTR`, which is the
normal consequence of a signal arriving.

`iox_loop_start` and `iox_loop_is_stopped` exist for hand-rolled loops built on
`iox_loop_poll`; `iox_loop_run` calls `iox_loop_start` itself.

The idle callback runs once per iteration of `iox_loop_run`, after dispatch.
It does not run in a loop driven by `iox_loop_poll` directly.

## iox_fd.h

```c
#define IOX_READ   (1u << 0)
#define IOX_WRITE  (1u << 1)
#define IOX_EXCEPT (1u << 2)

typedef void (*iox_fd_cb)(struct iox_loop *loop, int fd,
                          unsigned events, void *arg);

int  iox_fd_add(struct iox_loop *loop, int fd, unsigned events,
                iox_fd_cb cb, void *arg);
int  iox_fd_mod(struct iox_loop *loop, int fd, unsigned events);
void iox_fd_remove(struct iox_loop *loop, int fd);
```

`iox_fd_add` returns 0, or -1 when the table is full at 64 descriptors. It
does not check whether the descriptor is already watched; adding one twice
gives it two entries and two callbacks per readiness.

`iox_fd_mod` returns 0, or -1 if the descriptor is not being watched.

`iox_fd_remove` is safe during dispatch, including on the descriptor being
dispatched. It is silent about a descriptor it does not find.

The `events` a callback receives are what fired, not what was requested.
`POLLHUP`, `POLLERR`, and `POLLNVAL` all arrive as `IOX_READ`, on the
reasoning that the caller's read is what turns any of them into a specific
error it can act on.

`POLLERR` additionally sets `IOX_EXCEPT`, for callers that need to tell an
error condition from readable data without reading first. The Linux case
that needs it is `MSG_ZEROCOPY`, where send completions wait on the socket
error queue and `recvmsg(MSG_ERRQUEUE)` is the only way to collect them.
`IOX_READ` stays set alongside it, so code written before the flag existed
behaves exactly as it did.

`IOX_EXCEPT` is delivery only. `poll` cannot be asked for `POLLERR` and
reports it regardless. Passing `IOX_EXCEPT` is accepted and documents
intent, but changes nothing. `POLLNVAL` does not set it: a descriptor
closed behind the loop is a caller bug the loop already handles by
dropping the entry, not a condition to go read about.

A watcher that cares only about the error condition should still ask for
`IOX_READ`. macOS implements `poll` on top of kqueue and registers no
filter for an `events` of 0, so a watcher added that way never fires there
at all.

The flag itself is Linux and Windows. `WSAPoll` reports `POLLERR`, though
with no error queue on Windows it only ever means a pending socket error.
macOS maps kqueue's `EV_EOF` to `POLLHUP` and sets `POLLERR` only when
registering the event failed, so a pending error arrives as `POLLHUP` and
reaches the callback as plain `IOX_READ`. Code that wants the distinction
portably has to fall back to reading, or to `getsockopt(SO_ERROR)`, on
platforms that do not deliver it.

## iox_timer.h

```c
typedef void (*iox_timer_cb)(struct iox_loop *loop, void *arg);

int  iox_timer_add(struct iox_loop *loop, int ms, iox_timer_cb cb, void *arg);
void iox_timer_remove(struct iox_loop *loop, int id);
```

`iox_timer_add` returns an id `>= 0`, or -1 if the heap cannot grow. A
negative `ms` is treated as 0. Timers are one-shot; re-add from the callback
to repeat.

Ids are unique among *pending* timers and are recycled afterwards. The
counter wraps at `INT_MAX` rather than overflowing, and skips any value a
pending timer still holds.

`iox_timer_remove` accepts an expired or unknown id and does nothing with it.

Dispatch takes a snapshot of the deadline before it starts, so a callback
that re-arms with `ms == 0` runs on the next iteration rather than
immediately, and cannot spin inside the dispatch loop.

## iox_signal.h

```c
typedef void (*iox_signal_cb)(struct iox_loop *loop, int signo, void *arg);

int  iox_signal_add(struct iox_loop *loop, int signo, iox_signal_cb cb,
                    void *arg);
void iox_signal_remove(struct iox_loop *loop, int signo);
```

`iox_signal_add` returns 0, or -1 if this loop already holds 8 signals, if
the process already holds 8 distinct ones, or if installing the handler
failed. Registering a signal already on this loop replaces its callback and
returns 0.

`iox_signal_remove` drops this loop's registration. The process-wide
disposition is restored only when the last loop watching that signal drops
it, so removing on one loop does not disarm another.

Callbacks run in normal loop context, not in the signal handler.

## iox_version.h

- `IOX_VERSION_MAJOR`, `IOX_VERSION_MINOR`, `IOX_VERSION_PATCH` -- the three
  components, as integers.
- `IOX_VERSION_STRING` -- the same thing as text, `"0.2.1"` style.
- `IOX_VERSION` -- one comparable integer, `MAJOR * 10000 + MINOR * 100 +
  PATCH`, so 0.2.1 is 201. This is what a version guard tests.

A vendored copy has no git, so this header is what identifies it. It is also
the only place in the tree a version is written by hand: the handful of
copy-pasteable commands in the documentation are generated from it by
`tools/version.sh --sync`, and `--check` fails if they drift.

## Return conventions

Functions returning `int` return 0 for success and -1 for failure, except
`iox_loop_poll` and `iox_timer_add`, which return a non-negative count or id.
For those, `< 0` is the failure test. `iox_loop_new` returns `NULL` on
failure.

`iox_loop_poll` is the only function that leaves anything meaningful in
`errno`, because it is a thin wrapper over `poll`.
