# What a poll() loop cannot do

Working through [the UDP send paths](README.md) turned up a question worth
answering directly: what would libiox have to change to support GSO,
`MSG_ZEROCOPY`, and io_uring? The three answers are very different, and the
third one is the interesting one.

## GSO: nothing

UDP GSO is a control message on `sendmsg`. The application owns the socket
and the send call. The event loop never sees it. Worth stating plainly,
because GSO is the feature carrying the 6x and it costs the library
nothing.

## MSG_ZEROCOPY: one event flag

This was closer than it looked. `iox_loop_poll` dispatches on
`revents != 0` without masking against the requested events, so a watcher
added with `events` of 0 already gets woken on `POLLERR`. The plumbing was
there.

What was missing was discrimination. `iox_events_from_poll` folded
`POLLIN`, `POLLHUP`, `POLLERR`, and `POLLNVAL` all into `IOX_READ`, so a
zero-copy sender could not tell "the peer sent data" from "a completion is
waiting on the error queue" and would have to call `recvmsg(MSG_ERRQUEUE)`
speculatively on every wakeup.

`IOX_EXCEPT` fixes that in a few lines. It is set in addition to
`IOX_READ`, never instead of it, so the existing behaviour where an error
arrives as readable and the caller's next read fails visibly is unchanged.
That was a deliberate design decision and the new flag had no business
overturning it.

## io_uring: a different library

The mismatch is not the syscall. It is the abstraction.

### Readiness against completion

`iox_fd_cb(loop, fd, events, arg)` means "you may now call `read()`". The
application performs the I/O. io_uring means "I performed the I/O, here is
the result". There is no way to express `IORING_OP_SENDMSG_ZC` through
`iox_fd_add`, because `iox_fd_add` has no concept of submitting an
operation. It would take a second, parallel API:

```c
struct iox_op;
typedef void (*iox_op_cb)(struct iox_loop *loop, struct iox_op *op,
                          ssize_t res, unsigned flags, void *arg);
int iox_sendmsg(struct iox_loop *loop, int fd, const struct msghdr *msg,
                int flags, iox_op_cb cb, void *arg, struct iox_op **out);
```

Two things changed in that callback. It is keyed by an operation handle
rather than by `fd`, because a completion model has many operations
outstanding on one descriptor. And `res` carries a negative errno directly,
because `errno` means nothing at reap time. That is a different error
convention from `IOX_OK` / `IOX_ERR` plus `errno`, running alongside the
existing one in the same library.

### Cancellation stops being certain

This is the deepest problem, and it is not about performance.

libiox's safety story is deferred removal. `iox_fd_remove` marks the entry
inactive, `compact()` runs after dispatch finishes, and so removal from
inside a callback is safe, synchronous, and complete when it returns. That
works because the loop owns every piece of the state involved.

A submitted SQE is inside the kernel. It cannot be un-submitted.
Cancellation becomes `IORING_OP_ASYNC_CANCEL`, which is itself
asynchronous and can fail because the operation already completed. "Safe to
call from within a dispatch callback" becomes "the cancel is queued, you
will find out later, and the buffer is still pinned until you do". Every
caller has to handle a state the current API does not have.

### Buffer ownership inverts

libiox never touches application memory today. Completion I/O requires the
loop to hold buffers across the submission, and the two-CQE model means the
operation completing and the buffer becoming reusable are separate events.
Either the callback fires twice with different flags, or a second
`iox_buf_release_cb` appears. Getting the real benefit means registered
buffers, which puts a buffer pool inside a library that currently exports
twenty functions.

### Structure

`struct iox_loop` embeds `struct pollfd pfds[64]` inline, which is what
makes the loop a single allocation with no growth. A completion model needs
an operation table keyed by `user_data`, and outstanding operations are not
bounded by descriptor count. `iox_loop_poll` calls `poll()` directly, so
there would have to be a backend abstraction with submit, wait, and reap.
That is the libev shape, and it roughly doubles the library.

### What would not have to change

Timers and signals are cheap, which is worth knowing because they look like
they would be the hard part. `iox_timer_next_ms` already produces a timeout
that can be handed to `io_uring_enter` as a timespec, so the heap stays as
it is. The signal self-pipe stays and becomes a multishot read.

## The Windows angle, which cuts the other way

Counterintuitively, a completion API maps *better* to Windows than the
current readiness API does. IOCP and RIO are completion-based, and
`WSAPoll` is the awkward emulation the library is stuck with today. If the
goal were one design that fits io_uring and native Windows properly, it is
the completion model, and `poll()` is the odd one out.

That is an argument for a different library, not for retrofitting this one.

## Conclusion

`IOX_EXCEPT` was worth adding. The rest was not.

An application that genuinely needs io_uring has a working architecture
that requires no libiox change at all: run io_uring in its own thread
owning the data plane, keep libiox on the control plane for listeners,
timers, signals, and config reloads, and connect the two with an eventfd
registered through `iox_fd_add`. The hot path never touches `poll()` and
the cold path stays simple.

Retrofitting completion I/O onto libiox produces libuv, and libuv already
exists and is better at it. The value here is six files that can be read in
an afternoon and vendored without a build step. A submission and completion
API with an operation table, a buffer pool, and asynchronous cancellation
is not that library.
