/*
 * iox_fuzz.c : drive the libiox API from a byte stream
 *
 * The interesting bugs in an event loop are not in any single call, they are
 * in the order calls arrive: removing a descriptor from inside its own
 * dispatch, cancelling a timer that already fired, freeing a loop that still
 * holds signals. So the fuzz target is the API itself. Each input byte is an
 * opcode and the bytes after it are its arguments, which turns a corpus into
 * a corpus of call sequences.
 *
 * Builds two ways. With libFuzzer:
 *
 *     clang -fsanitize=fuzzer,address,undefined -Isrc \
 *         tests/iox_fuzz.c src/iox_*.c -o iox_fuzz
 *
 * Or standalone, to replay a corpus with no libFuzzer present, which is what
 * CI runs so that a crash found once stays found:
 *
 *     cc -DIOX_FUZZ_STANDALONE -Isrc tests/iox_fuzz.c src/iox_*.c -o iox_fuzz
 *     ./iox_fuzz tests/fuzz-corpus/
 */

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"

#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUZZ_MAX_PIPES  8
#define FUZZ_MAX_TIMERS 32
#define FUZZ_MAX_OPS    512

enum {
    OP_FD_ADD,
    OP_FD_MOD,
    OP_FD_REMOVE,
    OP_TIMER_ADD,
    OP_TIMER_REMOVE,
    OP_SIGNAL_ADD,
    OP_SIGNAL_REMOVE,
    OP_RAISE,
    OP_POLL,
    OP_PIPE_WRITE,
    OP_PIPE_DRAIN,
    OP_PIPE_CLOSE,
    OP_SET_IDLE,
    OP_COUNT,
};

struct fuzz_state {
    struct iox_loop *loop;
    int pipes[FUZZ_MAX_PIPES][2];
    int pipe_live[FUZZ_MAX_PIPES];
    int timer_ids[FUZZ_MAX_TIMERS];
    int timer_count;
    int reentrant_removes;
};

static struct fuzz_state st;

/* Callbacks do something observable and occasionally re-enter the API, since
 * removal from inside dispatch is the path most worth exercising. */
static void
fd_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    char buf[64];
    ssize_t n;

    (void)events;

    n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        iox_fd_remove(loop, fd);
        return;
    }

    if (arg && st.reentrant_removes > 0) {
        st.reentrant_removes--;
        iox_fd_remove(loop, fd);
    }
}

static void
timer_cb(struct iox_loop *loop, void *arg)
{
    /* Re-arm sometimes, which is how a caller builds a repeating timer and
     * the case that could spin dispatch if the deadline were re-read. */
    if (arg)
        iox_timer_add(loop, 0, timer_cb, NULL);
}

static void
signal_cb(struct iox_loop *loop, int signo, void *arg)
{
    (void)loop;
    (void)signo;
    (void)arg;
}

static void
idle_cb(struct iox_loop *loop, void *arg)
{
    (void)loop;
    (void)arg;
}

static void
state_teardown(void)
{
    int i;

    if (st.loop)
        iox_loop_free(st.loop);
    st.loop = NULL;

    for (i = 0; i < FUZZ_MAX_PIPES; i++) {
        if (st.pipe_live[i]) {
            if (st.pipes[i][0] >= 0)
                close(st.pipes[i][0]);
            if (st.pipes[i][1] >= 0)
                close(st.pipes[i][1]);
        }
        st.pipe_live[i] = 0;
        st.pipes[i][0] = -1;
        st.pipes[i][1] = -1;
    }

    st.timer_count = 0;
    st.reentrant_removes = 0;
}

static int
state_setup(void)
{
    int i;

    memset(&st, 0, sizeof(st));
    for (i = 0; i < FUZZ_MAX_PIPES; i++) {
        st.pipes[i][0] = -1;
        st.pipes[i][1] = -1;
    }

    st.loop = iox_loop_new();
    if (!st.loop)
        return -1;

    for (i = 0; i < FUZZ_MAX_PIPES; i++) {
        if (pipe(st.pipes[i]) != 0) {
            st.pipes[i][0] = -1;
            st.pipes[i][1] = -1;
            continue;
        }
        /* Non-blocking, because a callback here reads whatever fired and a
         * fuzzer that blocks in read() is a fuzzer that has hung. A real
         * caller of an event loop wants this too. */
        fcntl(st.pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(st.pipes[i][1], F_SETFL, O_NONBLOCK);
        st.pipe_live[i] = 1;
    }

    return 0;
}

/* Reading past the end yields zeros, so a truncated input is still a valid
 * (shorter) program rather than a rejected one. That keeps the corpus
 * minimisable. */
static unsigned char
take(const uint8_t **p, const uint8_t *end)
{
    if (*p >= end)
        return 0;
    return *(*p)++;
}

static void
do_poll(void)
{
    /* iox_loop_poll blocks until a descriptor is ready or a timer is due,
     * and a fuzzer must never block. A zero-millisecond timer guarantees a
     * deadline exists, so poll returns immediately either way. */
    iox_timer_add(st.loop, 0, timer_cb, NULL);
    (void)iox_loop_poll(st.loop);
}

static void
step(const uint8_t **p, const uint8_t *end)
{
    unsigned char op = take(p, end) % OP_COUNT;
    unsigned char a = take(p, end);
    unsigned char b = take(p, end);
    int idx = a % FUZZ_MAX_PIPES;
    int id;

    switch (op) {
    case OP_FD_ADD:
        if (st.pipe_live[idx]) {
            unsigned ev = (b & 1 ? IOX_READ : 0) | (b & 2 ? IOX_WRITE : 0);

            if (!ev)
                ev = IOX_READ;
            (void)iox_fd_add(st.loop, st.pipes[idx][b & 4 ? 1 : 0], ev,
                             fd_cb, (b & 8) ? &st : NULL);
        }
        break;
    case OP_FD_MOD:
        if (st.pipe_live[idx])
            (void)iox_fd_mod(st.loop, st.pipes[idx][0],
                             (b & 1) ? IOX_WRITE : IOX_READ);
        break;
    case OP_FD_REMOVE:
        if (st.pipe_live[idx])
            iox_fd_remove(st.loop, st.pipes[idx][b & 1]);
        break;
    case OP_TIMER_ADD:
        id = iox_timer_add(st.loop, b % 4, timer_cb, (a & 1) ? &st : NULL);
        if (id >= 0 && st.timer_count < FUZZ_MAX_TIMERS)
            st.timer_ids[st.timer_count++] = id;
        break;
    case OP_TIMER_REMOVE:
        if (st.timer_count > 0)
            iox_timer_remove(st.loop, st.timer_ids[a % st.timer_count]);
        else
            iox_timer_remove(st.loop, (int)a);   /* unknown id is legal */
        break;
    case OP_SIGNAL_ADD:
        (void)iox_signal_add(st.loop, (b & 1) ? SIGUSR1 : SIGUSR2,
                             signal_cb, NULL);
        break;
    case OP_SIGNAL_REMOVE:
        iox_signal_remove(st.loop, (b & 1) ? SIGUSR1 : SIGUSR2);
        break;
    case OP_RAISE:
        /* Safe because both signals are SIG_IGN at process level before any
         * loop exists, so an unregistered raise is a no-op rather than a
         * kill. libiox saves and restores that disposition. */
        raise((b & 1) ? SIGUSR1 : SIGUSR2);
        break;
    case OP_POLL:
        st.reentrant_removes = b & 3;
        do_poll();
        break;
    case OP_PIPE_WRITE:
        if (st.pipe_live[idx] && st.pipes[idx][1] >= 0) {
            ssize_t r = write(st.pipes[idx][1], "x", 1);

            (void)r;
        }
        break;
    case OP_PIPE_DRAIN:
        if (st.pipe_live[idx] && st.pipes[idx][0] >= 0) {
            char buf[32];
            ssize_t r = read(st.pipes[idx][0], buf, sizeof(buf));

            (void)r;
        }
        break;
    case OP_PIPE_CLOSE:
        /* Close behind the loop's back, without iox_fd_remove. This is the
         * POLLNVAL path: a bug here used to spin the loop forever. */
        if (st.pipe_live[idx]) {
            if (st.pipes[idx][0] >= 0) {
                close(st.pipes[idx][0]);
                st.pipes[idx][0] = -1;
            }
            if (st.pipes[idx][1] >= 0) {
                close(st.pipes[idx][1]);
                st.pipes[idx][1] = -1;
            }
            st.pipe_live[idx] = 0;
        }
        break;
    case OP_SET_IDLE:
        iox_loop_set_idle(st.loop, (b & 1) ? idle_cb : NULL, NULL);
        break;
    default:
        break;
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    static int signals_ignored;
    int ops = 0;

    if (!signals_ignored) {
        signal(SIGUSR1, SIG_IGN);
        signal(SIGUSR2, SIG_IGN);
        signals_ignored = 1;
    }

    if (size == 0)
        return 0;

    if (state_setup() != 0) {
        state_teardown();
        return 0;
    }

    while (p < end && ops++ < FUZZ_MAX_OPS)
        step(&p, end);

    state_teardown();
    return 0;
}

#ifdef IOX_FUZZ_STANDALONE

/* Replay a corpus without libFuzzer, so a regression found by fuzzing can be
 * checked in and re-run by anyone. */
int
main(int argc, char **argv)
{
    unsigned char buf[65536];
    int i;

    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n;

        if (!f) {
            fprintf(stderr, "iox_fuzz: cannot open %s\n", argv[i]);
            return 1;
        }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);

        LLVMFuzzerTestOneInput(buf, n);
        printf("  replayed %s (%zu bytes)\n", argv[i], n);
    }

    if (argc == 1) {
        /* No corpus: run a short deterministic sweep so the harness itself
         * is exercised on every build. */
        unsigned char seq[256];
        size_t k;

        for (k = 0; k < sizeof(seq); k++)
            seq[k] = (unsigned char)(k * 7 + 13);
        LLVMFuzzerTestOneInput(seq, sizeof(seq));
        printf("  built-in sweep ok\n");
    }

    printf("iox_fuzz: no crashes\n");
    return 0;
}

#endif /* IOX_FUZZ_STANDALONE */
