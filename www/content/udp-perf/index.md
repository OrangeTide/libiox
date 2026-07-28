---
title: Sending UDP fast, and where poll() stops helping
date: 2026-07-27
abstract: UDP GSO, MSG_ZEROCOPY, and io_uring's SENDMSG_ZC, measured against each other by one program that implements all four. Batching is worth 6x. Zero-copy is worth watching carefully before you trust it.
category: internals
tags: [linux, udp, io_uring, performance]
---

libiox is a `poll()` loop, and `poll()` is not how you push 40 Gbit/s of
UDP. That is worth saying out loud rather than working around. But the
question of what *does* make UDP fast on Linux has an answer that
applies to almost any program, including ones built on a plain event
loop, so here it is with numbers.

The short version: batching is what pays. Copy avoidance is a second
order effect that shows up only under specific conditions, and it can
easily cost more than it saves.

The measurement program lives in the repository at
[`docs/udp-perf/udpzc.c`](https://github.com/OrangeTide/libiox/blob/main/docs/udp-perf/udpzc.c).
It is Linux only, has no dependencies, and is not part of the libiox
build.

## Three mechanisms

### UDP GSO, one syscall for 64 datagrams

Attach a `UDP_SEGMENT` control message giving a segment size, then hand
the kernel one large buffer. It slices that into datagrams of that size,
up to 64 per call, as late in the stack as it can manage.

```c
struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
uint16_t gso_size = 1400;

cm->cmsg_level = SOL_UDP;
cm->cmsg_type = UDP_SEGMENT;
cm->cmsg_len = CMSG_LEN(sizeof(gso_size));
memcpy(CMSG_DATA(cm), &gso_size, sizeof(gso_size));
```

One syscall instead of 45, and one trip down the stack instead of 45.
This is the largest win available and the one with the fewest new
failure modes. `UDP_GRO` is the mirror image on the receive side.

### MSG_ZEROCOPY, and the bookkeeping it costs you

Set `SO_ZEROCOPY` on the socket, then pass `MSG_ZEROCOPY` to `sendmsg`.
The kernel pins your pages and links them into the skb instead of
copying. TCP got this in 4.14, UDP in 5.0.

The buffer is not yours again when `sendmsg` returns. You find out it is
free by watching for `POLLERR` and reading the socket error queue with
`recvmsg(fd, &msg, MSG_ERRQUEUE)`. The parts that are easy to get wrong:

- The notification carries an inclusive range `[ee_info, ee_data]` of
  send sequence numbers, because the kernel coalesces consecutive
  completions. One read can retire many sends.
- Ranges can arrive out of order. Record each sequence number. Do not
  assume FIFO and do not just keep a count.
- The counter advances once per *successful* `MSG_ZEROCOPY` send, per
  socket. A failed send does not advance it. Advance your own counter
  anyway and your buffer accounting is desynchronized permanently.
- `ee_code & SO_EE_CODE_ZEROCOPY_COPIED` means the kernel copied
  regardless. The documented advice is to stop using the flag on that
  socket.
- `ENOBUFS` means the socket optmem limit or `RLIMIT_MEMLOCK` ran out.
  Retry as an ordinary copying send.
- You must drain the error queue. Notifications accumulate otherwise and
  sends eventually start failing.

Copy avoidance quietly degrades to a copy on devices without
scatter-gather, when the kernel has to compute the checksum itself, and
on loopback to a local socket.

### io_uring SEND_ZC and SENDMSG_ZC

`IORING_OP_SEND_ZC` (6.0) and `IORING_OP_SENDMSG_ZC` (6.1) do the same
page pinning, but the notification comes back as an ordinary CQE.
`SO_ZEROCOPY` is not involved at all.

One submission produces two completions. The first is the send result,
and if it carries `IORING_CQE_F_MORE` a second is coming. The second
carries `IORING_CQE_F_NOTIF` and means the buffer is free. Both share
the same `user_data`. Freeing on the first one is the mistake to avoid.

Setting `IORING_SEND_ZC_REPORT_USAGE` makes the notification's `res`
field carry `IORING_NOTIF_USAGE_ZC_COPIED` when the kernel copied. That
is the only way to know you are paying for zero-copy without getting it,
and it is the io_uring counterpart of `SO_EE_CODE_ZEROCOPY_COPIED`.

Beyond speed, the model is simply better. Notifications are per request
rather than a shared per-socket counter, there is no second queue to
poll and drain, and completions arrive where you are already looking.

## What it measures

`udpzc.c` implements all four paths behind flags so they are directly
comparable, with GSO available on any of them:

| flags | path |
|---|---|
| (none) | `sendmsg` |
| `-z` | `sendmsg` with `MSG_ZEROCOPY` |
| `-u` | `IORING_OP_SENDMSG` |
| `-u -z` | `IORING_OP_SENDMSG_ZC` |

```sh
cc -O2 -Wall -Wextra -o udpzc udpzc.c
./udpzc -l -p 9999                              # receiver
./udpzc -h HOST -p 9999 -g -n 45 -c 200000      # sender
```

The io_uring path calls `io_uring_setup` and `io_uring_enter` directly
and maps the rings itself, so there is no liburing dependency and the
SQE and CQE handling is visible rather than hidden behind
`io_uring_prep_*`. Both zero-copy paths keep a ring of 64 buffers and
refuse to refill one until its completion has arrived, which is the part
an application actually has to build.

## Numbers

Loopback, Xeon E5-2650 v2, Linux 7.0, gcc 13.3. Best of three runs,
200000 datagrams each.

| send path | 1400 B datagrams | GSO, 45 x 1400 B | 64000 B datagrams |
|---|---|---|---|
| `sendmsg` | 1.44 | 8.83 | 21.76 |
| `sendmsg` + `MSG_ZEROCOPY` | 0.90 | 7.46 | 13.65 |
| `IORING_OP_SENDMSG` | 1.51 | 9.44 | 21.61 |
| `IORING_OP_SENDMSG_ZC` | 1.12 | 7.92 | 16.19 |

Gbit/s, higher is better.

**GSO is the win.** 1.44 to 8.83 Gbit/s, roughly 6x, from batching
alone. 4445 syscalls instead of 200000.

**io_uring is a modest submission-side gain**, 5 to 7 percent here. That
is the cost of the syscall itself, which GSO has already amortized away.
It counts for more when you cannot batch.

**Zero-copy loses in every column**, and that result is correct rather
than surprising. Loopback forces a deferred copy, and both zero-copy
paths reported that every completion came back marked copied. So those
numbers are page accounting plus notification overhead with no copy
avoided. This is exactly why the program reports the copied count, and
why these loopback figures cannot be used to argue against zero-copy on
real hardware.

The kernel documentation puts the break-even for `MSG_ZEROCOPY` around
10 KB of payload. A 1400 byte datagram is nowhere near it. A GSO
super-buffer at 63 KB is, which makes the two features complements
rather than alternatives. To find out whether zero-copy helps you, run
the sender across a real NIC with scatter-gather and checksum offload
and compare `-g` against `-g -z`. If the copied count stays at zero and
throughput does not move, the copy was not your bottleneck.

## Where libiox fits

**UDP GSO needs nothing from the event loop.** It is a control message
on `sendmsg`. Any libiox program can adopt it today, and it is the
change worth making first.

**`MSG_ZEROCOPY` fits, with one wrinkle.** Completion readiness arrives
as `POLLERR`, and `poll()` reports `POLLERR` in `revents` whether or not
you asked for it, so an existing `IOX_WRITE` watcher already wakes up.
The callback needs a second step that drains the error queue, and the
buffers-in-flight accounting lives in your application rather than in
the loop. That is a fair amount of work for a benefit that only appears
above roughly 10 KB per send.

**io_uring does not fit the `poll()` model.** You can register the ring
fd as a readable watcher and reap when it fires. That works, and it is
also paying a `poll()` wakeup per batch for a mechanism whose entire
purpose is avoiding syscalls. It is a compatibility shim, not a fast
path. A program that genuinely needs io_uring wants io_uring owning its
event loop, not sitting as a guest inside a `poll()` loop.

That is a real limit of a `poll()`-based design, and it is not worth
papering over. libiox is aimed at programs where a few hundred
descriptors and clear portable semantics matter more than peak packet
rate. A QUIC server saturating a 40 Gbit link is not that program, and
libiox should not pretend otherwise.

## References

- [MSG_ZEROCOPY, kernel documentation](https://docs.kernel.org/networking/msg_zerocopy.html)
- [io_uring_prep_sendmsg_zc(3)](https://man7.org/linux/man-pages/man3/io_uring_prep_sendmsg_zc.3.html)
- [Zero-copy network transmission with io_uring, LWN](https://lwn.net/Articles/879724/)
- [io_uring zerocopy send, LWN](https://lwn.net/Articles/877167/)
- [Accelerating UDP packet transmission for QUIC, Cloudflare](https://blog.cloudflare.com/accelerating-udp-packet-transmission-for-quic/)
- [sendmsg copy avoidance with MSG_ZEROCOPY, netdev 2.1](https://netdevconf.info/2.1/papers/debruijn-msgzerocopy-talk.pdf)
