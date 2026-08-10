---
title: Using libiox
date: 2026-07-27
abstract: A working event loop in about twenty lines. Watch descriptors, schedule timers, handle signals without writing anything into a signal handler.
category: guide
tags: [c, event-loop, poll]
---

libiox is a `poll()` loop with three things attached to it: descriptor
watchers, one-shot timers, and signal delivery. It allocates one object per
loop and fixed arrays inside it. There is no configuration and no build step.

## A loop that does something

```c
#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void
on_readable(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    char buf[256];
    ssize_t n;

    (void)events;
    (void)arg;

    n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        iox_fd_remove(loop, fd);
        return;
    }
    fwrite(buf, 1, (size_t)n, stdout);
}

static void
on_tick(struct iox_loop *loop, void *arg)
{
    (void)arg;
    fprintf(stderr, "still here\n");
    iox_timer_add(loop, 1000, on_tick, NULL);   /* one-shot: re-arm */
}

static void
on_signal(struct iox_loop *loop, int signo, void *arg)
{
    (void)signo;
    (void)arg;
    iox_loop_stop(loop);
}

int
main(void)
{
    struct iox_loop *loop = iox_loop_new();

    if (!loop)
        return 1;

    iox_fd_add(loop, STDIN_FILENO, IOX_READ, on_readable, NULL);
    iox_timer_add(loop, 1000, on_tick, NULL);
    iox_signal_add(loop, SIGINT, on_signal, NULL);

    iox_loop_run(loop);
    iox_loop_free(loop);
    return 0;
}
```

Build it with the five library sources and one include path. On Windows add
`-lws2_32`.

## Descriptors

`iox_fd_add` watches a descriptor for `IOX_READ`, `IOX_WRITE`, or both. The
callback receives the flags that actually fired, which is not necessarily
what you asked for: a hangup or an error arrives as `IOX_READ`, so the read
you were going to do anyway returns 0 or fails and you find out there.

An error condition also sets `IOX_EXCEPT`, which matters when reading is not
how you want to find out. The case that motivated it is `MSG_ZEROCOPY` on
Linux, where send completions queue on the socket error queue and have to be
collected with `recvmsg(MSG_ERRQUEUE)` rather than an ordinary read. You
cannot ask for it, since `poll` reports `POLLERR` whether or not it was
requested. Pass `IOX_READ` anyway, because macOS registers no kqueue
filter for an `events` of 0 and never polls such a descriptor. macOS also
reports these conditions as `POLLHUP` rather than `POLLERR`, so the flag
arrives on Linux and Windows only.

`iox_fd_remove` is safe to call from inside a dispatch callback, including on
the descriptor currently being dispatched. Removal during dispatch is
deferred and the table is compacted once dispatch finishes, so the iteration
never walks off the end of a table that shrank underneath it.

Closing a descriptor without removing it first is a bug, but a survivable
one. The loop reports it readable once, so the callback's next read fails
visibly, then drops it from the poll set. It will not spin.

## Timers

Timers are one-shot. They fire once and are removed, so a repeating timer
re-arms itself from its own callback, as `on_tick` does above. That is a
deliberate choice: a repeating timer that fires slower than its own callback
runs has to decide whether to drop, coalesce, or queue, and no single answer
is right for every caller. Re-arming makes the decision explicit.

`iox_timer_add` returns an id you can pass to `iox_timer_remove`. Ids are
non-negative and unique among pending timers. They are recycled once a timer
fires or is cancelled, so an id you are still holding after the timer fired
may name a different timer later. Do not keep one past the callback.

Deadlines come from a monotonic clock, so they survive a wall-clock jump.

## Signals

`iox_signal_add` registers a callback, not a signal handler. The callback
runs in normal loop context, so it may allocate, log, or call anything else.
The actual handler only writes one byte to a pipe the loop is already
watching, which is the whole trick.

Callbacks belong to a loop, but the signal *disposition* belongs to the
process, so several loops can watch the same signal and each gets its own
callback. The previous disposition is restored when the last loop watching
it drops it.

Registering a signal twice on the same loop replaces the callback rather than
stacking a second one.

## Driving the loop yourself

`iox_loop_run` is a convenience. If you already have a loop of your own, call
`iox_loop_poll` and drive it:

```c
iox_loop_start(loop);
while (!iox_loop_is_stopped(loop)) {
    if (iox_loop_poll(loop) < 0 && errno != EINTR)
        break;
    my_other_work();
}
```

`iox_loop_poll` blocks until a descriptor is ready or the next timer is due,
dispatches everything that came up, and returns the number of descriptor
events it dispatched. Timers that expired are dispatched too, but are not
counted in the return value.

## Limits

Everything is fixed-size except the timer heap, which grows:

- 64 descriptors per loop
- 8 signals per loop, and 8 distinct signals across the process
- 16 timers per loop to start with, doubling as needed

These are `#define`s at the top of their source files, not build options.
Change them in your copy if you need to; nothing else depends on the values.

libiox is not thread-safe. A loop and everything attached to it belongs to
one thread. Multiple loops in one process are fine, as long as each stays on
its own thread and the signal-watching ones cooperate.
