/*
 * iox_oom.c : fail every allocation in turn and check the unwind
 *
 * iox_loop_new allocates four times: the loop, the timer state, the timer
 * heap, and the signal state. Each failure has its own cleanup path, and
 * none of them run in a normal test, so a leak or a double free there would
 * sit undetected however long the suite runs under valgrind.
 *
 * The linker redirects this build's calloc to __wrap_calloc, which fails the
 * Nth call and passes the rest through. Sweeping N over the whole range walks
 * every unwind path exactly once. Run it under valgrind or ASan; on its own
 * it only checks that failure is reported rather than crashed on.
 *
 *     cc -Isrc -Wl,--wrap=calloc tests/iox_oom.c src/iox_*.c -o iox_oom
 */

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_calloc(size_t n, size_t size);

static int calloc_calls;
static int calloc_fail_at = -1;   /* -1 disables injection */

void *
__wrap_calloc(size_t n, size_t size)
{
    calloc_calls++;
    if (calloc_fail_at >= 0 && calloc_calls == calloc_fail_at)
        return NULL;
    return __real_calloc(n, size);
}

static int failures;

static void
check(int cond, const char *msg)
{
    if (!cond) {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

int
main(void)
{
    int n, allocs;

    /* One clean pass, to learn how many allocations a loop actually makes.
     * Hard-coding the count here would rot the moment the library changed. */
    calloc_fail_at = -1;
    calloc_calls = 0;
    {
        struct iox_loop *loop = iox_loop_new();

        check(loop != NULL, "baseline iox_loop_new failed");
        if (loop)
            iox_loop_free(loop);
    }
    allocs = calloc_calls;
    printf("iox_loop_new makes %d allocation(s)\n", allocs);
    check(allocs > 0, "no allocations observed -- is --wrap=calloc linked?");

    /* Fail each one in turn. Whichever it is, the result must be a clean
     * NULL rather than a partly built loop. */
    for (n = 1; n <= allocs; n++) {
        struct iox_loop *loop;

        calloc_calls = 0;
        calloc_fail_at = n;
        loop = iox_loop_new();
        calloc_fail_at = -1;

        printf("  calloc #%d fails -> %s\n", n, loop ? "loop built" : "NULL");
        check(loop == NULL, "iox_loop_new returned a loop despite OOM");
        if (loop)
            iox_loop_free(loop);
    }

    /* Past the last allocation nothing should fail, which confirms the sweep
     * covered the whole range rather than stopping early. */
    calloc_calls = 0;
    calloc_fail_at = allocs + 1;
    {
        struct iox_loop *loop = iox_loop_new();

        calloc_fail_at = -1;
        check(loop != NULL, "iox_loop_new failed past the last allocation");
        if (loop) {
            /* Exercise the loop so a half-initialised one would show up. */
            check(iox_timer_add(loop, 0, NULL, NULL) >= 0,
                  "timer_add failed on a healthy loop");
            (void)iox_loop_poll(loop);
            iox_loop_free(loop);
        }
    }

    printf("\niox_oom: %s\n", failures ? "FAILURES" : "all unwind paths clean");
    return failures ? 1 : 0;
}
