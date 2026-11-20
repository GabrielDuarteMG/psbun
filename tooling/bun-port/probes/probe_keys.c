/* PS5Term - probe_keys: how the PS5's libkernel runs pthread key destructors at thread exit
 * (order between keys, and whether a destructor that sets its key again gets another round,
 * as POSIX's PTHREAD_DESTRUCTOR_ITERATIONS allows). Decides how ps5compat's emulated TLS can
 * outlive the other thread-exit destructors. GPL-3.0-or-later. */
#include <pthread.h>
#include <stdatomic.h>

#include "probe.h"

#define NAME "probe_keys"

static pthread_key_t first, second;
static atomic_int order[16];
static atomic_int events;
static atomic_int first_rounds;

static void first_destructor(void *value)
{
    atomic_store(&order[atomic_fetch_add(&events, 1) & 15], 1);
    if (atomic_fetch_add(&first_rounds, 1) < 2)
        pthread_setspecific(first, value); /* Ask for another round. */
}

static void second_destructor(void *value)
{
    (void)value;
    atomic_store(&order[atomic_fetch_add(&events, 1) & 15], 2);
}

static void *worker(void *argument)
{
    (void)argument;
    pthread_setspecific(first, (void *)1);
    pthread_setspecific(second, (void *)2);
    return NULL;
}

int main(void)
{
    PROBE_START(NAME);
    if (pthread_key_create(&first, first_destructor) || pthread_key_create(&second, second_destructor))
        return PROBE_FAIL(NAME, "pthread_key_create", errno);
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, NULL))
        return PROBE_FAIL(NAME, "thread", errno);
    char text[64] = "";
    int count = atomic_load(&events);
    for (int i = 0; i < count && i < 16; i++)
        text[i] = (char)('0' + atomic_load(&order[i]));
    PROBE_LOG(NAME, "destructor order %s (1=first key, 2=second key), first-key rounds %d", text,
              atomic_load(&first_rounds));
    return PROBE_PASS(NAME);
}
