# High-rate UDP sending on Linux

Notes from looking at what it takes to push UDP fast on Linux, and a
test program that measures the four combinations. libiox is a `poll()`
loop and is not the right tool for the fastest option here, but the
tradeoffs matter to anything sending a lot of small datagrams, so they
are written down.

The short version: batching is what pays. Copy avoidance is a second
order effect that only shows up under specific conditions, and it can
easily cost you more than it saves.

## Contents

- [udpzc.c](udpzc.c) -- the test program. Linux only, no dependencies,
  including no liburing.

```sh
cc -O2 -Wall -Wextra -o udpzc udpzc.c
./udpzc -l -p 9999                              # receiver
./udpzc -h HOST -p 9999 -g -n 45 -c 200000      # sender
```

## The three mechanisms

### UDP GSO

Attach a `UDP_SEGMENT` control message to `sendmsg` giving a segment
size, then hand the kernel one large buffer. The kernel slices it into
datagrams of that size, up to 64 per call. Segmentation happens as late
as possible, in the driver on hardware that supports it.

```c
struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
uint16_t gso_size = 1400;

cm->cmsg_level = SOL_UDP;
cm->cmsg_type = UDP_SEGMENT;
cm->cmsg_len = CMSG_LEN(sizeof(gso_size));
memcpy(CMSG_DATA(cm), &gso_size, sizeof(gso_size));
```

This is one syscall instead of 45, and one trip down the stack instead
of 45. It is by far the largest single win available, and it is also the
easiest to adopt. The receiving side has `UDP_GRO` as the mirror image.

### MSG_ZEROCOPY

Set `SO_ZEROCOPY` on the socket, then pass `MSG_ZEROCOPY` to `sendmsg`.
The kernel pins the user pages and links them into the skb `frags[]`
array instead of copying. Available for TCP since 4.14 and for UDP since
5.0.

The catch is that the buffer is not reusable when `sendmsg` returns. You
find out it is free by watching for `POLLERR` and reading the socket
error queue:

```c
recvmsg(fd, &msg, MSG_ERRQUEUE);
/* cmsg IP_RECVERR / IPV6_RECVERR, ee_origin == SO_EE_ORIGIN_ZEROCOPY */
```

Things that are easy to get wrong:

- The notification carries an inclusive range `[ee_info, ee_data]` of
  send sequence numbers, because the kernel coalesces consecutive
  completions. One read can retire many sends.
- Ranges can arrive out of order. Record each sequence number. Do not
  assume FIFO and do not just count.
- The counter increments once per successful `MSG_ZEROCOPY` send call,
  per socket. A send that fails does not advance it. If you advance your
  own counter anyway, your buffer accounting desynchronizes permanently.
- `ee_code & SO_EE_CODE_ZEROCOPY_COPIED` means the kernel copied anyway.
  The documented advice is to stop using the flag on that socket.
- `ENOBUFS` means the socket optmem limit or `RLIMIT_MEMLOCK` is
  exhausted. Retry as an ordinary copying send.
- You must drain the error queue. Notifications accumulate otherwise and
  sends eventually fail.

Copy avoidance silently degrades to a copy for devices without
scatter-gather, when the kernel has to compute the checksum itself, and
for loopback to a local socket.

### io_uring SEND_ZC and SENDMSG_ZC

`IORING_OP_SEND_ZC` (6.0) and `IORING_OP_SENDMSG_ZC` (6.1) do the same
page pinning, but the notification arrives as an ordinary CQE instead of
on the socket error queue. `SO_ZEROCOPY` is not involved.

One submission produces two completions:

1. The send result. If it has `IORING_CQE_F_MORE`, a second is coming.
2. A completion with `IORING_CQE_F_NOTIF`, meaning the buffer is now
   free.

Both carry the same `user_data`, so you match them yourself. Freeing on
the first one is the mistake to avoid.

Setting `IORING_SEND_ZC_REPORT_USAGE` in `sqe->ioprio` makes the
notification's `res` field carry `IORING_NOTIF_USAGE_ZC_COPIED` when the
kernel copied. That is the equivalent of `SO_EE_CODE_ZEROCOPY_COPIED`
and it is the only way to know you are paying for zero-copy without
getting it.

`SEND_ZC` also accepts `IORING_RECVSEND_FIXED_BUF` to send from
pre-registered buffers, which removes the per-send pinning cost.
`SENDMSG_ZC` does not have that, which matters because unconnected UDP
needs the `msghdr` to carry the destination address.

Compared to `MSG_ZEROCOPY` the model is better in ways that go beyond
speed. Notifications are per request rather than a shared per-socket
sequence counter, there is no separate error queue to poll and drain,
and completions land in the same queue you are already reaping.

## Measurements

Loopback, Xeon E5-2650 v2, Linux 7.0, gcc 13.3. Best of three runs,
200000 datagrams each. Loopback numbers are only good for comparing
syscall and batching costs against each other. They cannot show
zero-copy in a good light, because loopback forces a deferred copy.

| send path | 1400 B datagrams | GSO, 45 x 1400 B | 64000 B datagrams |
|---|---|---|---|
| `sendmsg` | 1.44 | 8.83 | 21.76 |
| `sendmsg` + `MSG_ZEROCOPY` | 0.90 | 7.46 | 13.65 |
| `IORING_OP_SENDMSG` | 1.51 | 9.44 | 21.61 |
| `IORING_OP_SENDMSG_ZC` | 1.12 | 7.92 | 16.19 |

Gbit/s, higher is better.

What this says:

- **GSO is the win.** 1.44 to 8.83 Gbit/s, roughly 6x, from batching
  alone. 4445 syscalls instead of 200000. Nothing else in the table
  comes close, and it is the change with the least new failure modes.
- **io_uring is a modest submission-side gain**, around 5 to 7 percent
  here. That is the cost of the syscall itself, which GSO has already
  mostly amortized away. It matters more when you cannot batch.
- **Zero-copy loses on loopback, in every column.** Both zero-copy paths
  reported that all completions came back marked copied, so the numbers
  are measuring pure overhead: page accounting plus the notification
  round trip with no copy avoided. This is the expected result, and it
  is exactly why the program reports the copied count.

The kernel documentation puts the break-even for `MSG_ZEROCOPY` at
around 10 KB of payload. A 1400 byte datagram is nowhere near that. The
GSO super-buffer at 63 KB is what puts you over the line, so on real
hardware the two features are complements rather than alternatives.

To see whether zero-copy helps you, run the sender across a real NIC
with scatter-gather and checksum offload and compare `-g` against
`-g -z`. If the copied count stays at zero and throughput does not move,
the copy was not your bottleneck at that size.

## The test program

`udpzc.c` implements all four paths behind flags so they are directly
comparable:

| flags | path |
|---|---|
| (none) | `sendmsg` |
| `-z` | `sendmsg` with `MSG_ZEROCOPY` |
| `-u` | `IORING_OP_SENDMSG` |
| `-u -z` | `IORING_OP_SENDMSG_ZC` |

`-g` adds GSO to any of them, `-n` sets segments per send, `-s` sets
segment size, `-c` sets the datagram count. `-l` runs a receiver, with
`-G` to enable GRO.

Both zero-copy paths keep a ring of 64 buffers and refuse to refill a
buffer until its completion has arrived, which is the part an
application actually has to build. The io_uring path talks to the three
syscalls directly and maps the rings itself, so there is no liburing
dependency and the SQE and CQE handling is visible rather than hidden
behind `io_uring_prep_*`.

## Where libiox fits

Nothing here needs libiox, and the fastest option does not fit it well.
Recording the boundary anyway:

**GSO needs nothing from the event loop.** It is a cmsg on `sendmsg`.
Any libiox program can adopt it today, and it is the change worth making
first.

**`MSG_ZEROCOPY` fits, with one wrinkle.** Completion readiness shows up
as `POLLERR`, and `poll()` reports `POLLERR` in `revents` whether or not
you asked for it, so an `IOX_WRITE` watcher already gets woken. The
callback needs a second step that drains the error queue, and the
buffer-in-flight accounting lives in the application rather than the
loop. That is a reasonable amount of work for a benefit that only
appears above roughly 10 KB per send.

**io_uring does not fit the `poll()` model.** You can register the ring
fd as a readable watcher and reap when it fires, and that works, but you
are paying a `poll()` wakeup per batch for a mechanism whose entire
point is avoiding syscalls. It is a compatibility shim, not a fast path.
An application that genuinely needs io_uring wants io_uring to own its
event loop, not to be a guest inside a `poll()` loop.

That is a real limit of a `poll()`-based design and not something to
paper over. libiox is aimed at programs where a few hundred descriptors
and clear portable semantics matter more than peak packet rate. A QUIC
server pushing 40 Gbit/s is not that program.

## References

- [MSG_ZEROCOPY, kernel documentation](https://docs.kernel.org/networking/msg_zerocopy.html)
- [io_uring_prep_sendmsg_zc(3)](https://man7.org/linux/man-pages/man3/io_uring_prep_sendmsg_zc.3.html)
- [io_uring_prep_send_zc(3)](https://man7.org/linux/man-pages/man3/io_uring_prep_send_zc.3.html)
- [Zero-copy network transmission with io_uring, LWN](https://lwn.net/Articles/879724/)
- [io_uring zerocopy send, LWN](https://lwn.net/Articles/877167/)
- [Accelerating UDP packet transmission for QUIC, Cloudflare](https://blog.cloudflare.com/accelerating-udp-packet-transmission-for-quic/)
- [sendmsg copy avoidance with MSG_ZEROCOPY, netdev 2.1](https://netdevconf.info/2.1/papers/debruijn-msgzerocopy-talk.pdf)
