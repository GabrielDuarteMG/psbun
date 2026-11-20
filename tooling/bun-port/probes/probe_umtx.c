/* PS5Term - Bun port probe: _umtx_op WAIT_UINT_PRIVATE / WAKE_PRIVATE between threads,
 * as Bun's Futex (src/threading/Futex.rs) uses them. A ping-pong of 20000 rounds with a
 * 2 s wait timeout; a timed-out wait means a lost wakeup. GPL-3.0-or-later. */
#include <sys/types.h>
#include <sys/umtx.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "probe.h"

#define ROUNDS 20000

static _Atomic unsigned turn; /* even: main's turn, odd: worker's */
static atomic_int timeouts, wait_errors;

static void wait_while(unsigned value)
{
    while (atomic_load(&turn) == value)
    {
        struct _umtx_time timeout = {._timeout = {2, 0}, ._flags = 0, ._clockid = CLOCK_MONOTONIC};
        int r = _umtx_op((void *)&turn, UMTX_OP_WAIT_UINT_PRIVATE, value,
                         (void *)sizeof timeout, &timeout);
        if (r == -1 && errno == ETIMEDOUT)
            atomic_fetch_add(&timeouts, 1);
        else if (r == -1 && errno != EINTR)
            atomic_fetch_add(&wait_errors, 1);
    }
}

static void advance(void)
{
    atomic_fetch_add(&turn, 1);
    _umtx_op((void *)&turn, UMTX_OP_WAKE_PRIVATE, 1, NULL, NULL);
}

static void *worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < ROUNDS; i++)
    {
        wait_while(2 * i);
        advance();
    }
    return NULL;
}

int main(void)
{
    PROBE_START("probe_umtx");
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    for (unsigned i = 0; i < ROUNDS; i++)
    {
        advance();
        wait_while(2 * i + 1);
    }
    pthread_join(thread, NULL);
    PROBE_LOG("probe_umtx", "rounds=%d timeouts=%d wait_errors=%d last_errno=%d", ROUNDS,
              atomic_load(&timeouts), atomic_load(&wait_errors), errno);
    return atomic_load(&timeouts) || atomic_load(&wait_errors)
               ? PROBE_FAIL("probe_umtx", "lost wakeups", EAGAIN)
               : PROBE_PASS("probe_umtx");
}
