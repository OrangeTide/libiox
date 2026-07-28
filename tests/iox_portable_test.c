/*
 * iox_portable_test.c : the subset of libiox that can be tested anywhere
 *
 * iox_test.c is the real suite, and it is POSIX: pipes, sigaction, SIGUSR1,
 * fcntl. None of that exists on Windows, so the Windows half of the library
 * has only ever been compiled, never run. That was the honest gap in this
 * library's testing.
 *
 * This file closes it. It uses only what both platforms have: UDP sockets on
 * loopback, SIGINT and SIGTERM from the C runtime, and timers. It runs
 * natively on Linux and Windows, and under wine against the mingw build,
 * which is what tools/wine-test.sh does.
 *
 * Winsock needs WSAStartup before any socket call. iox_loop_new does that on
 * Windows as part of bringing up signal delivery, so every socket here is
 * created after the loop exists.
 */

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"
#include "iox_version.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define CLOSESOCK closesocket
#  define BAD_SOCK  INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
#  define CLOSESOCK close
#  define BAD_SOCK  (-1)
#endif

static int test_count;
static int fail_count;

#define TEST(name) \
    do { \
        test_count++; \
        printf("  %-52s ", name); \
        fflush(stdout); \
    } while (0)

#define PASS() do { printf("ok\n"); } while (0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
        fail_count++; \
    } while (0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            FAIL(msg); \
            return; \
        } \
    } while (0)

/* A UDP socket bound to an ephemeral loopback port, which can be made
 * readable by sending to its own address. The nearest portable thing to a
 * self-pipe. */
static sock_t
selfsock(struct sockaddr_in *addr)
{
    sock_t s;
    socklen_t len = sizeof(*addr);

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCK)
        return BAD_SOCK;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin_port = 0;

    if (bind(s, (struct sockaddr *)addr, sizeof(*addr)) != 0 ||
        getsockname(s, (struct sockaddr *)addr, &len) != 0) {
        CLOSESOCK(s);
        return BAD_SOCK;
    }

    return s;
}

static void
poke(sock_t s, const struct sockaddr_in *addr)
{
    (void)sendto(s, "x", 1, 0, (const struct sockaddr *)addr, sizeof(*addr));
}

static void
drain(sock_t s)
{
    char buf[64];

    (void)recvfrom(s, buf, (int)sizeof(buf), 0, NULL, NULL);
}

/* ---- loop lifecycle ---- */

static void
test_loop_new_free(void)
{
    struct iox_loop *loop;

    TEST("loop new/free");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "iox_loop_new returned NULL");
    iox_loop_free(loop);
    PASS();
}

static void
test_is_stopped(void)
{
    struct iox_loop *loop;

    TEST("iox_loop_is_stopped tracks start and stop");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    ASSERT(iox_loop_is_stopped(loop), "a fresh loop should read as stopped");
    iox_loop_start(loop);
    ASSERT(!iox_loop_is_stopped(loop), "loop_start did not clear it");
    iox_loop_stop(loop);
    ASSERT(iox_loop_is_stopped(loop), "loop_stop did not set it");

    iox_loop_free(loop);
    PASS();
}

/* ---- socket readability ---- */

static int sock_cb_called;
static int sock_cb_events;

static void
sock_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    (void)arg;
    sock_cb_called++;
    sock_cb_events = (int)events;
    drain((sock_t)fd);
    iox_loop_stop(loop);
}

static void
test_socket_read(void)
{
    struct iox_loop *loop;
    struct sockaddr_in addr;
    sock_t s;

    TEST("socket read event");

    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    s = selfsock(&addr);
    ASSERT(s != BAD_SOCK, "cannot bind a loopback socket");

    ASSERT(iox_fd_add(loop, (int)s, IOX_READ, sock_cb, NULL) == IOX_OK,
           "fd_add failed");

    sock_cb_called = 0;
    sock_cb_events = 0;
    poke(s, &addr);

    /* A timer bounds the wait, so a lost datagram fails the test rather
     * than hanging the run. */
    ASSERT(iox_timer_add(loop, 2000, NULL, NULL) >= 0, "timer_add failed");
    ASSERT(iox_loop_poll(loop) >= 0, "poll failed");

    ASSERT(sock_cb_called == 1, "socket callback not called");
    ASSERT(sock_cb_events & IOX_READ, "IOX_READ not set");

    iox_loop_free(loop);
    CLOSESOCK(s);
    PASS();
}

/* ---- timers ---- */

static int timer_fired;

static void
timer_cb(struct iox_loop *loop, void *arg)
{
    (void)loop;
    (void)arg;
    timer_fired++;
}

static void
test_timer_fires(void)
{
    struct iox_loop *loop;

    TEST("timer fires");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    timer_fired = 0;
    ASSERT(iox_timer_add(loop, 0, timer_cb, NULL) >= 0, "timer_add failed");
    (void)iox_loop_poll(loop);
    ASSERT(timer_fired == 1, "timer callback not called");

    iox_loop_free(loop);
    PASS();
}

static int order_seen[2];
static int order_idx;

static void
order_cb(struct iox_loop *loop, void *arg)
{
    (void)loop;
    if (order_idx < 2)
        order_seen[order_idx++] = *(int *)arg;
}

static void
test_timer_order(void)
{
    struct iox_loop *loop;
    int first = 1, second = 2;

    TEST("timers fire in deadline order");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    order_idx = 0;
    ASSERT(iox_timer_add(loop, 60, order_cb, &second) >= 0, "add long");
    ASSERT(iox_timer_add(loop, 0, order_cb, &first) >= 0, "add short");

    (void)iox_loop_poll(loop);
    ASSERT(order_idx >= 1, "no timer fired");
    ASSERT(order_seen[0] == 1, "the shorter timer did not fire first");

    iox_loop_free(loop);
    PASS();
}

static void
test_timer_remove(void)
{
    struct iox_loop *loop;
    int id;

    TEST("timer_remove cancels a pending timer");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    timer_fired = 0;
    id = iox_timer_add(loop, 0, timer_cb, NULL);
    ASSERT(id >= 0, "timer_add failed");
    iox_timer_remove(loop, id);

    ASSERT(iox_timer_add(loop, 10, NULL, NULL) >= 0, "timer_add failed");
    (void)iox_loop_poll(loop);
    ASSERT(timer_fired == 0, "a removed timer still fired");

    iox_loop_free(loop);
    PASS();
}

/* ---- signals ----
 *
 * SIGINT and SIGTERM are the two the Windows C runtime actually has. raise()
 * runs the handler on the calling thread on both platforms, so the datagram
 * or pipe byte is already queued by the time poll runs. */

static int signal_seen;
static int signal_signo;

static void
signal_cb(struct iox_loop *loop, int signo, void *arg)
{
    (void)arg;
    signal_seen++;
    signal_signo = signo;
    iox_loop_stop(loop);
}

static void
test_signal(void)
{
    struct iox_loop *loop;

    TEST("signal delivery reaches the callback");
    loop = iox_loop_new();
    ASSERT(loop != NULL, "loop new failed");

    ASSERT(iox_signal_add(loop, SIGTERM, signal_cb, NULL) == IOX_OK,
           "signal_add failed");

    signal_seen = 0;
    signal_signo = 0;
    raise(SIGTERM);

    ASSERT(iox_timer_add(loop, 2000, NULL, NULL) >= 0, "timer_add failed");
    (void)iox_loop_poll(loop);

    ASSERT(signal_seen == 1, "signal callback not called");
    ASSERT(signal_signo == SIGTERM, "wrong signal number delivered");

    iox_signal_remove(loop, SIGTERM);
    iox_loop_free(loop);
    PASS();
}

static int loop_a_seen;
static int loop_b_seen;

static void
loop_a_cb(struct iox_loop *loop, int signo, void *arg)
{
    (void)loop; (void)signo; (void)arg;
    loop_a_seen++;
}

static void
loop_b_cb(struct iox_loop *loop, int signo, void *arg)
{
    (void)loop; (void)signo; (void)arg;
    loop_b_seen++;
}

static void
test_two_loops(void)
{
    struct iox_loop *a, *b;

    TEST("two loops receive the same signal independently");
    a = iox_loop_new();
    ASSERT(a != NULL, "loop a new failed");
    b = iox_loop_new();
    ASSERT(b != NULL, "loop b new failed");

    ASSERT(iox_signal_add(a, SIGTERM, loop_a_cb, NULL) == IOX_OK, "add on a");
    ASSERT(iox_signal_add(b, SIGTERM, loop_b_cb, NULL) == IOX_OK, "add on b");

    loop_a_seen = 0;
    loop_b_seen = 0;
    raise(SIGTERM);

    ASSERT(iox_timer_add(a, 2000, NULL, NULL) >= 0, "timer on a");
    ASSERT(iox_timer_add(b, 2000, NULL, NULL) >= 0, "timer on b");
    (void)iox_loop_poll(a);
    (void)iox_loop_poll(b);

    ASSERT(loop_a_seen == 1, "loop a did not receive the signal");
    ASSERT(loop_b_seen == 1, "loop b did not receive the signal");

    iox_loop_free(a);
    iox_loop_free(b);
    PASS();
}

static void
test_free_one_keeps_other(void)
{
    struct iox_loop *a, *b;

    TEST("freeing one loop leaves the other's signals armed");
    a = iox_loop_new();
    ASSERT(a != NULL, "loop a new failed");
    b = iox_loop_new();
    ASSERT(b != NULL, "loop b new failed");

    ASSERT(iox_signal_add(b, SIGTERM, loop_b_cb, NULL) == IOX_OK, "add on b");
    iox_loop_free(a);

    loop_b_seen = 0;
    raise(SIGTERM);
    ASSERT(iox_timer_add(b, 2000, NULL, NULL) >= 0, "timer on b");
    (void)iox_loop_poll(b);

    ASSERT(loop_b_seen == 1, "loop b lost its handler when a was freed");

    iox_loop_free(b);
    PASS();
}

/* ---- main ---- */

int
main(void)
{
    printf("libiox %s portable tests", IOX_VERSION_STRING);
#ifdef _WIN32
    printf(" (windows)\n");
#else
    printf(" (posix)\n");
#endif

    test_loop_new_free();
    test_is_stopped();
    test_socket_read();
    test_timer_fires();
    test_timer_order();
    test_timer_remove();
    test_signal();
    test_two_loops();
    test_free_one_keeps_other();

    printf("\n%d tests, %d failures\n", test_count, fail_count);
    return fail_count ? 1 : 0;
}
