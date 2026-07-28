/* udpzc.c : UDP GSO send test -- sendmsg, MSG_ZEROCOPY, io_uring */
/* PUBLIC DOMAIN (CC0-1.0) */

/* Build:
 *     cc -O2 -Wall -Wextra -o udpzc udpzc.c
 *
 * No liburing needed. The io_uring path talks to the kernel directly.
 *
 * Receiver:  ./udpzc -l -p 9999
 * Senders:   ./udpzc -h HOST -p 9999 -g            plain sendmsg
 *            ./udpzc -h HOST -p 9999 -g -z         MSG_ZEROCOPY
 *            ./udpzc -h HOST -p 9999 -g -u         IORING_OP_SENDMSG
 *            ./udpzc -h HOST -p 9999 -g -u -z      IORING_OP_SENDMSG_ZC
 *
 * Loopback forces a deferred copy, so measure across a real NIC if you
 * want to see zero-copy take effect. Both zero-copy paths report how
 * many completions came back marked as copied. */

#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <poll.h>

#include <linux/errqueue.h>
#include <linux/io_uring.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif
#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY 5
#endif
#ifndef SO_EE_CODE_ZEROCOPY_COPIED
#define SO_EE_CODE_ZEROCOPY_COPIED 1
#endif

#define OK 0
#define ERR (-1)

/* Number of send buffers in flight. A buffer stays pinned until its
 * completion arrives, so it cannot be refilled before then. */
#define DEPTH 64

/* Kernel caps a GSO super-buffer at this many segments. */
#define MAX_SEGS 64

#define MAX_PAYLOAD 65507

/* io_uring submission queue depth, and how many SQEs to batch per
 * io_uring_enter. */
#define URING_ENTRIES 256
#define URING_BATCH 16

struct stats {
    uint64_t calls;             // submissions the kernel accepted
    uint64_t datagrams;         // datagrams handed to the kernel
    uint64_t bytes;
    uint64_t notif;             // zero-copy completions seen
    uint64_t notif_copied;      // completions flagged as copied
    uint64_t enobufs;           // sends that fell back to a copy
    uint64_t short_sends;
};

/* One in-flight send. The msghdr and its control buffer have to outlive
 * the submission for io_uring, so they live here rather than on the
 * stack. */
struct slot {
    char *buf;
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(uint16_t))];
    size_t len;
    unsigned segs;
};

struct uring {
    int fd;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    void *sq_ring, *cq_ring;
    size_t sq_ring_sz, cq_ring_sz, sqes_sz;
    unsigned sq_entries;
};

struct sender {
    int fd;
    int family;
    int zerocopy;
    int gso;
    int uring;
    size_t seg;
    unsigned nseg;
    size_t buflen;
    struct slot slots[DEPTH];

    /* MSG_ZEROCOPY bookkeeping */
    uint32_t next_seq;          // kernel's per-socket zero-copy counter
    uint32_t outstanding;
    int64_t done_seq[DEPTH];

    /* io_uring bookkeeping */
    struct uring ring;
    unsigned free_idx[DEPTH];
    unsigned free_top;
    int failed;

    struct stats st;
};

static volatile sig_atomic_t stop_flag;

static void
on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/** Install a handler that interrupts blocking calls.
 *
 * signal() installs with SA_RESTART on glibc, which would restart the
 * blocking recv() and make ^C useless in the receiver.
 */
static void
catch_signal(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            // no SA_RESTART
    sigaction(sig, &sa, NULL);
}

static double
now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/****************************************************************
 * Shared message setup
 ****************************************************************/

/** Point a slot's msghdr at its buffer, adding a GSO cmsg if asked.
 *
 * With GSO the kernel slices one buffer into segs datagrams of seg
 * bytes each. Without it the buffer is a single datagram.
 */
static void
slot_prepare(struct sender *s, struct slot *sl, size_t len, unsigned segs)
{
    struct cmsghdr *cm;
    uint16_t gso_size = (uint16_t)s->seg;

    sl->len = len;
    sl->segs = segs;
    sl->iov.iov_base = sl->buf;
    sl->iov.iov_len = len;

    memset(&sl->msg, 0, sizeof(sl->msg));
    sl->msg.msg_iov = &sl->iov;
    sl->msg.msg_iovlen = 1;

    if (!s->gso || segs <= 1)
        return;

    memset(sl->control, 0, sizeof(sl->control));
    sl->msg.msg_control = sl->control;
    sl->msg.msg_controllen = sizeof(sl->control);
    cm = CMSG_FIRSTHDR(&sl->msg);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    memcpy(CMSG_DATA(cm), &gso_size, sizeof(gso_size));
}

/****************************************************************
 * MSG_ZEROCOPY completion handling
 ****************************************************************/

/** Read one batch of completions off the socket error queue.
 *
 * Each notification carries an inclusive range [ee_info, ee_data] of
 * send sequence numbers, because the kernel coalesces consecutive
 * completions. Ranges can arrive out of order, so record each sequence
 * number rather than assuming FIFO.
 *
 * Returns the number of sequence numbers retired, or 0 if the queue was
 * empty.
 */
static int
zc_reap(struct sender *s)
{
    char control[256];
    struct msghdr msg;
    struct cmsghdr *cm;
    struct sock_extended_err *serr;
    uint32_t lo, hi, seq;
    int retired = 0;

    memset(&msg, 0, sizeof(msg));
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    if (recvmsg(s->fd, &msg, MSG_ERRQUEUE) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        perror("recvmsg(MSG_ERRQUEUE)");
        return 0;
    }

    for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        int ok4 = cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR;
        int ok6 = cm->cmsg_level == SOL_IPV6 &&
                  cm->cmsg_type == IPV6_RECVERR;

        if (!ok4 && !ok6)
            continue;
        serr = (struct sock_extended_err *)CMSG_DATA(cm);
        if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY)
            continue;

        lo = serr->ee_info;
        hi = serr->ee_data;
        for (seq = lo; ; seq++) {
            s->done_seq[seq % DEPTH] = (int64_t)seq;
            retired++;
            if (seq == hi)
                break;
        }
        s->st.notif += (uint64_t)(hi - lo) + 1;
        if (serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED)
            s->st.notif_copied += (uint64_t)(hi - lo) + 1;
    }
    s->outstanding -= (uint32_t)retired;
    return retired;
}

/** Wait until the buffer slot for sequence number seq is free.
 *
 * Slot i last held sequence seq - DEPTH. That send must have completed
 * before the buffer can be refilled.
 */
static void
zc_wait_slot(struct sender *s, uint32_t seq)
{
    struct pollfd pfd;
    unsigned idx = seq % DEPTH;

    if (seq < DEPTH)
        return;

    while (s->done_seq[idx] != (int64_t)seq - DEPTH) {
        if (zc_reap(s) > 0)
            continue;
        pfd.fd = s->fd;
        pfd.events = 0;         // POLLERR is reported regardless
        pfd.revents = 0;
        if (poll(&pfd, 1, 1000) < 0 && errno != EINTR) {
            perror("poll");
            return;
        }
        if (stop_flag)
            return;
    }
}

static void
zc_drain_all(struct sender *s)
{
    struct pollfd pfd;

    while (s->outstanding > 0 && !stop_flag) {
        if (zc_reap(s) > 0)
            continue;
        pfd.fd = s->fd;
        pfd.events = 0;
        pfd.revents = 0;
        if (poll(&pfd, 1, 1000) <= 0)
            break;              // give up rather than hang on a lost notif
    }
}

/****************************************************************
 * sendmsg path
 ****************************************************************/

/** Send one slot. Returns bytes accepted, -2 if it fell back to a
 * copying send (so no completion is coming), or ERR.
 */
static ssize_t
send_one(struct sender *s, struct slot *sl)
{
    int flags = s->zerocopy ? MSG_ZEROCOPY : 0;
    ssize_t n;

    n = sendmsg(s->fd, &sl->msg, flags);
    if (n < 0 && s->zerocopy && errno == ENOBUFS) {
        /* Socket optmem limit or RLIMIT_MEMLOCK exhausted. Retire what
         * we can and retry as an ordinary copying send. */
        s->st.enobufs++;
        zc_reap(s);
        n = sendmsg(s->fd, &sl->msg, 0);
        if (n >= 0)
            return -2;
    }
    if (n < 0)
        return ERR;
    return n;
}

static int
run_sender_sendmsg(struct sender *s, uint64_t want)
{
    uint64_t sent = 0;

    while (sent < want && !stop_flag) {
        uint64_t left = want - sent;
        unsigned segs = s->gso ? s->nseg : 1;
        struct slot *sl;
        ssize_t n;

        if ((uint64_t)segs > left)
            segs = (unsigned)left;

        if (s->zerocopy) {
            zc_wait_slot(s, s->next_seq);
            if (stop_flag)
                break;
            sl = &s->slots[s->next_seq % DEPTH];
        } else {
            sl = &s->slots[s->st.calls % DEPTH];
        }
        slot_prepare(s, sl, s->seg * segs, segs);

        n = send_one(s, sl);
        if (n == ERR) {
            perror("sendmsg");
            return ERR;
        }
        if (n != -2 && s->zerocopy) {
            s->next_seq++;
            s->outstanding++;
        }
        if (n >= 0 && (size_t)n != sl->len)
            s->st.short_sends++;
        s->st.calls++;
        s->st.datagrams += segs;
        s->st.bytes += n == -2 ? sl->len : (uint64_t)n;
        sent += segs;

        if (s->zerocopy)
            zc_reap(s);         // opportunistic, never blocks
    }

    if (s->zerocopy)
        zc_drain_all(s);
    return OK;
}

/****************************************************************
 * io_uring
 ****************************************************************/

static int
io_uring_setup_sys(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int
io_uring_enter_sys(int fd, unsigned to_submit, unsigned min_complete,
                   unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, NULL, 0);
}

static int
uring_init(struct uring *r, unsigned entries)
{
    struct io_uring_params p;
    void *sq, *cq;

    memset(&p, 0, sizeof(p));
    memset(r, 0, sizeof(*r));

    r->fd = io_uring_setup_sys(entries, &p);
    if (r->fd < 0) {
        perror("io_uring_setup");
        return ERR;
    }

    r->sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r->cq_ring_sz = p.cq_off.cqes +
                    p.cq_entries * sizeof(struct io_uring_cqe);
    r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);

    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (r->cq_ring_sz > r->sq_ring_sz)
            r->sq_ring_sz = r->cq_ring_sz;
        r->cq_ring_sz = r->sq_ring_sz;
    }

    sq = mmap(NULL, r->sq_ring_sz, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
    if (sq == MAP_FAILED) {
        perror("mmap(sq)");
        close(r->fd);
        return ERR;
    }
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cq = sq;
    } else {
        cq = mmap(NULL, r->cq_ring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
        if (cq == MAP_FAILED) {
            perror("mmap(cq)");
            munmap(sq, r->sq_ring_sz);
            close(r->fd);
            return ERR;
        }
    }
    r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        perror("mmap(sqes)");
        munmap(sq, r->sq_ring_sz);
        if (cq != sq)
            munmap(cq, r->cq_ring_sz);
        close(r->fd);
        return ERR;
    }

    r->sq_ring = sq;
    r->cq_ring = cq;
    r->sq_entries = p.sq_entries;
    r->sq_head = (unsigned *)((char *)sq + p.sq_off.head);
    r->sq_tail = (unsigned *)((char *)sq + p.sq_off.tail);
    r->sq_mask = (unsigned *)((char *)sq + p.sq_off.ring_mask);
    r->sq_array = (unsigned *)((char *)sq + p.sq_off.array);
    r->cq_head = (unsigned *)((char *)cq + p.cq_off.head);
    r->cq_tail = (unsigned *)((char *)cq + p.cq_off.tail);
    r->cq_mask = (unsigned *)((char *)cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)((char *)cq + p.cq_off.cqes);
    return OK;
}

static void
uring_fini(struct uring *r)
{
    if (r->sqes && r->sqes != MAP_FAILED)
        munmap(r->sqes, r->sqes_sz);
    if (r->cq_ring && r->cq_ring != r->sq_ring)
        munmap(r->cq_ring, r->cq_ring_sz);
    if (r->sq_ring)
        munmap(r->sq_ring, r->sq_ring_sz);
    if (r->fd >= 0)
        close(r->fd);
}

/** Claim the next submission queue entry, or NULL if the queue is full.
 */
static struct io_uring_sqe *
uring_get_sqe(struct uring *r)
{
    unsigned tail = *r->sq_tail;
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned idx;

    if (tail - head >= r->sq_entries)
        return NULL;
    idx = tail & *r->sq_mask;
    r->sq_array[idx] = idx;
    return &r->sqes[idx];
}

static void
uring_advance_sqe(struct uring *r)
{
    __atomic_store_n(r->sq_tail, *r->sq_tail + 1, __ATOMIC_RELEASE);
}

static void
slot_free(struct sender *s, unsigned idx)
{
    s->free_idx[s->free_top++] = idx;
    s->outstanding--;
}

/** Drain the completion queue. With wait set, block for at least one
 * completion first. Returns the number of CQEs consumed.
 */
static int
uring_reap(struct sender *s, int wait)
{
    struct uring *r = &s->ring;
    unsigned head, tail;
    int seen = 0;

    if (wait) {
        if (io_uring_enter_sys(r->fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
            errno != EINTR && errno != EAGAIN) {
            perror("io_uring_enter(wait)");
            return 0;
        }
    }

    head = *r->cq_head;
    tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

    for (; head != tail; head++) {
        struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_mask];
        unsigned idx = (unsigned)cqe->user_data;
        struct slot *sl = &s->slots[idx];

        seen++;
        if (cqe->flags & IORING_CQE_F_NOTIF) {
            /* The buffer is only reusable now, not when the send
             * completed. */
            s->st.notif++;
            if (cqe->res & (int)IORING_NOTIF_USAGE_ZC_COPIED)
                s->st.notif_copied++;
            slot_free(s, idx);
            continue;
        }

        if (cqe->res < 0) {
            if (-cqe->res == ENOBUFS) {
                s->st.enobufs++;
            } else if (!s->failed) {
                errno = -cqe->res;
                perror(s->zerocopy ? "sendmsg_zc" : "sendmsg");
                s->failed = 1;  // report once, stop queueing, then drain
            }
        } else {
            s->st.calls++;
            s->st.datagrams += sl->segs;
            s->st.bytes += (uint64_t)cqe->res;
            if ((size_t)cqe->res != sl->len)
                s->st.short_sends++;
        }
        /* No IORING_CQE_F_MORE means no notification is coming, so the
         * buffer is free already. */
        if (!(cqe->flags & IORING_CQE_F_MORE))
            slot_free(s, idx);
    }
    __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    return seen;
}

static int
run_sender_uring(struct sender *s, uint64_t want)
{
    struct uring *r = &s->ring;
    uint64_t queued = 0;
    unsigned pending = 0;
    unsigned i;

    for (i = 0; i < DEPTH; i++)
        s->free_idx[i] = DEPTH - 1 - i;
    s->free_top = DEPTH;

    while (queued < want && !stop_flag && !s->failed) {
        uint64_t left = want - queued;
        unsigned segs = s->gso ? s->nseg : 1;
        struct io_uring_sqe *sqe;
        struct slot *sl;
        unsigned idx;

        if ((uint64_t)segs > left)
            segs = (unsigned)left;

        while (s->free_top == 0 && !stop_flag) {
            if (pending) {
                io_uring_enter_sys(r->fd, pending, 0, 0);
                pending = 0;
            }
            uring_reap(s, 1);
        }
        if (stop_flag)
            break;

        sqe = uring_get_sqe(r);
        if (!sqe) {
            io_uring_enter_sys(r->fd, pending, 0, 0);
            pending = 0;
            continue;
        }

        idx = s->free_idx[--s->free_top];
        sl = &s->slots[idx];
        slot_prepare(s, sl, s->seg * segs, segs);

        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = s->zerocopy ? IORING_OP_SENDMSG_ZC
                                  : IORING_OP_SENDMSG;
        sqe->fd = s->fd;
        sqe->addr = (uint64_t)(uintptr_t)&sl->msg;
        sqe->len = 1;
        sqe->msg_flags = 0;
        sqe->user_data = idx;
        if (s->zerocopy)
            sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
        uring_advance_sqe(r);

        s->outstanding++;
        pending++;
        queued += segs;

        if (pending >= URING_BATCH) {
            if (io_uring_enter_sys(r->fd, pending, 0, 0) < 0) {
                perror("io_uring_enter");
                return ERR;
            }
            pending = 0;
            uring_reap(s, 0);   // opportunistic, never blocks
        }
    }

    if (pending)
        io_uring_enter_sys(r->fd, pending, 0, 0);
    while (s->outstanding > 0 && !stop_flag)
        uring_reap(s, 1);
    return s->failed ? ERR : OK;
}

/****************************************************************
 * Sender driver and reporting
 ****************************************************************/

static void
report(struct sender *s, double secs)
{
    const char *how;

    if (s->uring)
        how = s->zerocopy ? "io_uring SENDMSG_ZC" : "io_uring SENDMSG";
    else
        how = s->zerocopy ? "sendmsg MSG_ZEROCOPY" : "sendmsg";

    if (secs <= 0.0)
        secs = 1e-9;

    printf("mode          %s%s\n", how, s->gso ? " + gso" : "");
    printf("segment       %zu bytes\n", s->seg);
    if (s->gso)
        printf("segs/call     %u (%zu byte super-buffer)\n", s->nseg,
               s->seg * s->nseg);
    printf("submissions   %llu\n", (unsigned long long)s->st.calls);
    printf("datagrams     %llu\n", (unsigned long long)s->st.datagrams);
    printf("bytes         %llu\n", (unsigned long long)s->st.bytes);
    printf("elapsed       %.3f s\n", secs);
    printf("throughput    %.2f Gbit/s (%.0f datagrams/s)\n",
           (double)s->st.bytes * 8.0 / secs / 1e9,
           (double)s->st.datagrams / secs);

    if (s->zerocopy) {
        printf("completions   %llu (%llu copied by the kernel)\n",
               (unsigned long long)s->st.notif,
               (unsigned long long)s->st.notif_copied);
        printf("ENOBUFS       %llu\n",
               (unsigned long long)s->st.enobufs);
        if (!s->uring && s->st.notif != s->next_seq)
            printf("WARNING: %u sends, %llu completions\n", s->next_seq,
                   (unsigned long long)s->st.notif);
        if (s->st.notif > 0 && s->st.notif_copied == s->st.notif)
            printf("NOTE: every send was copied. Loopback, a NIC "
                   "without scatter-gather, or no checksum offload.\n");
    }
    if (s->st.short_sends)
        printf("short sends   %llu\n",
               (unsigned long long)s->st.short_sends);
}

static int
run_sender(struct sender *s, uint64_t want)
{
    double t0, t1;
    unsigned i;
    int rc;

    for (i = 0; i < DEPTH; i++) {
        s->slots[i].buf = malloc(s->buflen);
        if (!s->slots[i].buf) {
            fprintf(stderr, "out of memory\n");
            return ERR;
        }
        memset(s->slots[i].buf, 'a' + (int)(i % 26), s->buflen);
        s->done_seq[i] = -1;
    }

    if (s->uring && uring_init(&s->ring, URING_ENTRIES) != OK) {
        for (i = 0; i < DEPTH; i++)
            free(s->slots[i].buf);
        return ERR;
    }

    t0 = now_sec();
    rc = s->uring ? run_sender_uring(s, want) : run_sender_sendmsg(s, want);
    t1 = now_sec();

    if (rc == OK)
        report(s, t1 - t0);

    if (s->uring)
        uring_fini(&s->ring);
    for (i = 0; i < DEPTH; i++)
        free(s->slots[i].buf);
    return rc;
}

/****************************************************************
 * Receiving
 ****************************************************************/

static int
run_receiver(int fd, int gro)
{
    static char buf[MAX_PAYLOAD];
    uint64_t pkts = 0, bytes = 0;
    double t0 = 0.0, t1;
    int one = 1;

    if (gro && setsockopt(fd, SOL_UDP, UDP_GRO, &one, sizeof(one)) < 0)
        perror("setsockopt(UDP_GRO)");

    while (!stop_flag) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;       // stop_flag is rechecked by the loop
            perror("recv");
            break;
        }
        if (pkts == 0)
            t0 = now_sec();
        pkts++;
        bytes += (uint64_t)n;
    }
    t1 = now_sec();

    if (pkts > 0 && t1 > t0) {
        printf("received      %llu reads, %llu bytes in %.3f s\n",
               (unsigned long long)pkts, (unsigned long long)bytes,
               t1 - t0);
        printf("throughput    %.2f Gbit/s\n",
               (double)bytes * 8.0 / (t1 - t0) / 1e9);
    } else {
        printf("received      nothing\n");
    }
    return OK;
}

/****************************************************************
 * Setup
 ****************************************************************/

static int
udp_socket(const char *host, const char *port, int family, int listening,
           int *out_family)
{
    struct addrinfo hints, *res, *ai;
    int fd = ERR, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (listening)
        hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return ERR;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (listening) {
            if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                *out_family = ai->ai_family;
                break;
            }
        } else {
            /* Connect so GSO and the error queue have a fixed peer. */
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                *out_family = ai->ai_family;
                break;
            }
        }
        close(fd);
        fd = ERR;
    }
    freeaddrinfo(res);

    if (fd < 0)
        perror(listening ? "bind" : "connect");
    return fd;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-l] [-h host] [-p port] [-4|-6] [-z] [-u] [-g]\n"
            "          [-G] [-s segsize] [-n segs] [-c datagrams]"
            " [-b bytes]\n"
            "\n"
            "  -l          receive instead of send\n"
            "  -h host     destination host (default 127.0.0.1)\n"
            "  -p port     port (default 9999)\n"
            "  -4 / -6     force address family\n"
            "  -z          zero-copy send\n"
            "  -u          submit through io_uring instead of sendmsg\n"
            "  -g          enable UDP GSO (UDP_SEGMENT)\n"
            "  -G          enable UDP GRO on the receiver\n"
            "  -s size     bytes per datagram (default 1400)\n"
            "  -n segs     segments per send with -g (default 64,"
            " max %d)\n"
            "  -c count    datagrams to send (default 100000)\n"
            "  -b bytes    SO_SNDBUF size\n"
            "\n"
            "  -z alone uses MSG_ZEROCOPY, -z -u uses"
            " IORING_OP_SENDMSG_ZC.\n",
            argv0, MAX_SEGS);
}

int
main(int argc, char *argv[])
{
    struct sender s;
    const char *host = "127.0.0.1";
    const char *port = "9999";
    uint64_t count = 100000;
    int family = AF_UNSPEC;
    int listening = 0, gro = 0, sndbuf = 0;
    int opt, one = 1, rc;

    memset(&s, 0, sizeof(s));
    s.seg = 1400;
    s.nseg = MAX_SEGS;

    while ((opt = getopt(argc, argv, "lh:p:46zugGs:n:c:b:?")) != -1) {
        switch (opt) {
        case 'l': listening = 1; break;
        case 'h': host = optarg; break;
        case 'p': port = optarg; break;
        case '4': family = AF_INET; break;
        case '6': family = AF_INET6; break;
        case 'z': s.zerocopy = 1; break;
        case 'u': s.uring = 1; break;
        case 'g': s.gso = 1; break;
        case 'G': gro = 1; break;
        case 's': s.seg = (size_t)strtoul(optarg, NULL, 0); break;
        case 'n': s.nseg = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'c': count = strtoull(optarg, NULL, 0); break;
        case 'b': sndbuf = (int)strtol(optarg, NULL, 0); break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (s.seg == 0 || s.seg > MAX_PAYLOAD) {
        fprintf(stderr, "segment size must be 1..%d\n", MAX_PAYLOAD);
        return 1;
    }
    if (s.nseg == 0 || s.nseg > MAX_SEGS) {
        fprintf(stderr, "segments per send must be 1..%d\n", MAX_SEGS);
        return 1;
    }
    if (s.gso && s.seg * s.nseg > MAX_PAYLOAD) {
        s.nseg = (unsigned)(MAX_PAYLOAD / s.seg);
        fprintf(stderr, "note: capped to %u segments per send\n", s.nseg);
    }

    catch_signal(SIGINT);
    catch_signal(SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    s.fd = udp_socket(listening ? NULL : host, port, family, listening,
                      &s.family);
    if (s.fd < 0)
        return 1;

    if (sndbuf > 0 &&
        setsockopt(s.fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
                   sizeof(sndbuf)) < 0)
        perror("setsockopt(SO_SNDBUF)");

    if (listening) {
        printf("listening on port %s, ^C to stop\n", port);
        rc = run_receiver(s.fd, gro);
        close(s.fd);
        return rc == OK ? 0 : 1;
    }

    /* io_uring zero-copy does not use SO_ZEROCOPY. Its notifications
     * come back as CQEs, not on the socket error queue. */
    if (s.zerocopy && !s.uring &&
        setsockopt(s.fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) < 0) {
        perror("setsockopt(SO_ZEROCOPY)");
        fprintf(stderr, "MSG_ZEROCOPY needs Linux 5.0 or newer for UDP\n");
        close(s.fd);
        return 1;
    }

    s.buflen = s.seg * (s.gso ? s.nseg : 1);
    rc = run_sender(&s, count);
    close(s.fd);
    return rc == OK ? 0 : 1;
}
