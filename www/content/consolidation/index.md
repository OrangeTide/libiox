---
title: Fourteen copies, one library
date: 2026-07-23
abstract: libiox was vendored into fourteen trees and drifted in four files. What the merge kept, and the four bugs every single copy shared.
category: history
tags: [vendoring, bugs, postmortem]
---

libiox existed as fourteen copies across unrelated projects before this
repository. Every copy was a `cp -R` of whichever version was to hand, and
each tree then edited its own. This is what the survey found.

## What actually diverged

Six of the fourteen were byte-identical to another copy. Of the rest, only
four files had ever diverged at all: `iox_signal.c`, `iox_timer.c`, `pq.h`,
and the build fragment, plus `iox_loop.c` in exactly one tree.

That is the encouraging part of the result. Fourteen copies drifting for a
year produced remarkably little real divergence, because almost nobody was
editing the library. They were carrying it.

The oldest copy predated timers entirely and still had the original
`iox_loop_poll(loop, timeout_ms)` signature. The newest had grown Windows
support. Everything in between differed in whitespace, in one warning
workaround, and in how a single `write()` return value was discarded.

## What the merge took

**Windows support**, from the newest copy, which had reached it by depending
on its host project's platform library. That dependency has no place in a
library whose whole pitch is that it depends on nothing, so the three
functions libiox actually used became `iox_plat.c`.

**The self-pipe write.** Four spellings of the same line existed across the
copies. `(void)write(...)` does not silence glibc's `warn_unused_result`, so
the assign-then-discard form won.

**The `pq.h` warning fix.** Two copies wrapped the include in a
`#pragma GCC diagnostic`; one had solved it properly by marking `PQ__API` as
`static __attribute__((unused))`. The proper fix won and the pragmas went.

**The test suite**, which survived in exactly one copy, and was also the only
one that covered timers.

## The four bugs

Every copy shared these. Each was reproduced before being fixed, and each is
now a test.

### Per-loop state was not per-loop

The timer heap, the signal table, the self-pipe, and the id counter were all
file-scope. Every `loop` argument in the timer and signal code was
`(void)loop`. The API had looked per-loop for a year and never was.

Two loops in one process therefore did this:

```
signal registered on loop b, delivered: 0   (expected 1)
freeing loop a...
[process killed by SIGUSR1]
```

The second loop never received signals, because init saw the already-open
shared pipe and returned early. And freeing *any* loop restored `SIG_DFL` for
handlers belonging to every other loop.

Nobody hit this because nobody ran two loops. That is the only reason it
survived fourteen copies.

### POLLNVAL span the loop at 100% CPU

`iox_events_from_poll` mapped `POLLIN`, `POLLHUP`, and `POLLERR`, but not
`POLLNVAL`. A descriptor closed without `iox_fd_remove` therefore produced no
event, no dispatch, and an instant return from `poll` on every iteration,
forever, with no log line and no error.

### iox_loop_run never checked errno

The comment said EINTR was handled. The code continued on *any* `poll`
failure, with no backoff, which is the same silent spin by a different route.

### Timer ids overflowed

A plain `int`, incremented forever, never recycled. Signed overflow is
undefined, and a negative id is indistinguishable from the -1 that means
failure. At a thousand timers a second that is about twenty-four days, which
is well within the uptime of a long-running server.

## What to take from it

The bugs were not subtle and none of them needed a fork to find. They needed
somebody to run the library in a configuration other than the one it happened
to be written for, and to build it with warnings turned up. Fourteen copies
meant fourteen chances for that, and it did not happen, because a vendored
copy is something you carry rather than something you read.

The mitigation is not fewer copies. Copying is the point of a library this
size. The mitigation is a real release mechanism, so a copy can say what it
is and an upgrade is one command. That is what `tools/vendor.sh` and
`iox_version.h` are for, and they exist because of this.
