---
title: How it works
date: 2026-07-25
abstract: The dispatch loop, the deferred-removal trick, why signal state is split in two, and what the Windows port has to do differently.
category: internals
tags: [c, internals, portability]
---

Six translation units. `iox_loop.c` owns the poll and dispatch, `iox_fd.c`
the descriptor table, `iox_timer.c` a binary heap, `iox_signal.c` the signal
plumbing, `iox_plat.c` the platform differences, and `pq.h` the heap itself.

## The loop object

Everything a loop owns lives in one allocation:

```c
struct iox_loop {
    struct pollfd       pfds[IOX_MAX_FDS];
    struct iox_fd_entry fds[IOX_MAX_FDS];
    int nfds, running, dispatching, compact_needed;
    iox_idle_cb idle_cb;
    void *idle_arg;
    struct iox_timers  *timers;    /* owned by iox_timer.c */
    struct iox_signals *signals;   /* owned by iox_signal.c */
};
```

The two parallel arrays are indexed together: `pfds[i]` is what `poll` sees
and `fds[i]` is the callback that goes with it. Keeping them separate means
the array handed to `poll` is exactly the array `poll` wants, with no
gather step.

Timer and signal state hang off opaque pointers rather than sitting inline.
Their layouts would drag `pq.h`, `signal.h`, and `winsock2.h` into every file
that includes `iox_internal.h`, and nothing outside their own translation
unit has any business looking inside them.

## Removal during dispatch

A callback is allowed to remove descriptors, including its own, while the
dispatch loop is walking the table. Compacting the array underneath that walk
would move entries the loop has not visited yet.

So removal during dispatch only clears a flag:

```c
if (loop->dispatching) {
    loop->fds[i].active = 0;
    loop->compact_needed = 1;
} else {
    /* swap with last */
}
```

The dispatch loop skips inactive entries, and compaction runs once dispatch
finishes. Compaction swaps the last entry into the hole and re-checks that
index rather than advancing, which handles a run of removed entries in one
pass.

Outside dispatch, removal compacts immediately, so the common case pays
nothing for the machinery.

## Timers

A min-heap keyed on a monotonic deadline in nanoseconds. `iox_timer_next_ms`
reads the root to decide how long `poll` should block, which is why the loop
never needs a tick.

Cancellation is a linear scan for the id, then a heap removal at that index.
That is O(n) in the number of pending timers, which is the right trade at
this scale: a heap indexed by id as well as deadline costs a second structure
and a great deal more code to save nothing measurable on sixteen entries.

Dispatch snapshots `now` before it starts and only fires timers due at or
before that instant. Without the snapshot, a callback re-arming with `ms == 0`
would land before a freshly-read `now` and fire again in the same pass,
forever.

Ids wrap at `INT_MAX` and skip values still held by pending timers. Signed
overflow is undefined behaviour, and a negative id would be
indistinguishable from the -1 that means failure.

## Signals

This is the only part with genuinely awkward shape, because a signal
disposition is a property of the process while a callback is a property of a
loop. The two cannot live in the same structure.

They are split. Each loop owns a `struct iox_signals` with its own self-pipe
and its own callback table. A process-wide table refcounts the dispositions:

```
        sig_disp[]                 sig_list
  signo, refs, old_sa   ------>  loop A signals -> loop B signals -> NULL
```

The handler writes the signal number to *every* registered loop's pipe. Each
loop then dispatches only the signals in its own table, so a loop that does
not care about `SIGUSR1` wakes up, reads a byte, matches nothing, and goes
back to sleep. That costs a wakeup and buys a design with no routing table in
the signal handler.

`disp_ref` installs the shared handler the first time a signal is asked for
and bumps a count afterwards. `disp_unref` restores the saved disposition
when the count reaches zero. Handler installation and removal happen outside
the handler, and the loop list is relinked under `sigprocmask`, so the
handler never walks a half-updated list.

The handler itself does the minimum that is async-signal-safe: a `write` per
loop, with `errno` saved and restored around it so it cannot corrupt the
value the interrupted code was about to read.

## Windows

There is no self-pipe on Windows. Anonymous pipes are not pollable by
`WSAPoll`, and the CRT runs the `SIGINT` handler on a separate thread.

So the Windows build binds a UDP socket to loopback and has the handler send
it a one-byte datagram. `WSAPoll` can wait on that, and the datagram carries
the signal number the same way the pipe byte does. The handler also re-arms
itself, because the CRT resets the disposition to `SIG_DFL` on delivery.

The rest of the port is `iox_plat.c`: `WSAPoll` for `poll`,
`QueryPerformanceCounter` for the monotonic clock, `ioctlsocket` for
non-blocking mode. The scaling in the clock splits whole seconds from the
sub-second remainder rather than multiplying the raw counter by a billion,
which would overflow 64 bits in a few hours.

One type mismatch is worth knowing about. `struct pollfd::fd` is a `SOCKET`
on Windows, an unsigned pointer-sized handle, while the libiox API speaks
`int`. Comparisons cast through `iox_pollfd_t` to say so explicitly. The
round trip is safe because every descriptor libiox holds arrived through the
API as an `int` in the first place.

The Windows objects are cross-compiled in CI with
`x86_64-w64-mingw32-gcc -Wall -Wextra -Werror`. The signal path is not
runtime-tested, and that is the honest state of it.

## What is deliberately absent

No epoll or kqueue backend. `poll` is the portable one, and at 64
descriptors the difference is not measurable. A loop that needs ten thousand
connections needs a different library, not a bigger version of this one.

No thread safety. Adding it would mean a lock on every dispatch to protect
against a case the library is not built for.

No dynamic descriptor table. The bound is 64 and it is a `#define` you can
read, which is worth more than a table that grows silently until something
else runs out.
