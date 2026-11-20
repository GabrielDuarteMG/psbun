/* PS5Term - probe_tls: _Thread_local storage, pthreads and a signal delivered to one thread.
 * GPL-3.0-or-later. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>

#include "probe.h"

#define NAME "probe_tls"
#define THREADS 4

static _Thread_local long sentinel = -1;
static _Thread_local volatile sig_atomic_t got_signal;
static atomic_int ready;
static atomic_int signalled_ok;

static void on_usr1(int signal_number)
{
    (void)signal_number;
    got_signal = 1;
}

static void *worker(void *argument)
{
    long id = (long)argument;
    if (sentinel != -1)
        return (void *)1;
    sentinel = id * 1000 + 7;
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) < THREADS)
        sched_yield();
    for (int i = 0; i < 100; i++)
    {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
        if (sentinel != id * 1000 + 7)
            return (void *)2;
        if (got_signal && atomic_load(&signalled_ok) == 0)
            atomic_store(&signalled_ok, (int)id + 1);
    }
    return sentinel == id * 1000 + 7 ? NULL : (void *)3;
}

int main(void)
{
    PROBE_START(NAME);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_usr1;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL))
        return PROBE_FAIL(NAME, "sigaction", errno);
    sentinel = 42;
    pthread_t threads[THREADS];
    for (long i = 0; i < THREADS; i++)
    {
        int error = pthread_create(&threads[i], NULL, worker, (void *)i);
        if (error)
            return PROBE_FAIL(NAME, "pthread_create", error);
    }
    while (atomic_load(&ready) < THREADS)
        sched_yield();
    int error = pthread_kill(threads[2], SIGUSR1);
    if (error)
        return PROBE_FAIL(NAME, "pthread_kill", error);
    for (int i = 0; i < THREADS; i++)
    {
        void *result = NULL;
        if ((error = pthread_join(threads[i], &result)))
            return PROBE_FAIL(NAME, "pthread_join", error);
        if (result)
        {
            PROBE_LOG(NAME, "thread %d sentinel check %ld", i, (long)result);
            return PROBE_FAIL(NAME, "thread-local value", EINVAL);
        }
    }
    if (sentinel != 42)
        return PROBE_FAIL(NAME, "main thread-local value", EINVAL);
    if (atomic_load(&signalled_ok) != 3)
    {
        PROBE_LOG(NAME, "signal seen by thread slot %d (want 3)", atomic_load(&signalled_ok));
        return PROBE_FAIL(NAME, "SIGUSR1 thread delivery", EINVAL);
    }
    PROBE_LOG(NAME, "4 threads, thread-local sentinels intact, SIGUSR1 in thread 2");
    return PROBE_PASS(NAME);
}
