/* PS5Term - probe_event: kqueue woken by a ps5compat eventfd written from another thread, and
 * a 50 ms kqueue timer. GPL-3.0-or-later. */
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/event.h>
#include <time.h>

#include "probe.h"
#include "ps5compat.h"

#define NAME "probe_event"

static int efd;

static void *writer(void *argument)
{
    (void)argument;
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
    uint64_t one = 1;
    return write(efd, &one, sizeof one) == sizeof one ? NULL : (void *)1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void)
{
    PROBE_START(NAME);
    int kq = kqueue();
    if (kq < 0)
        return PROBE_FAIL(NAME, "kqueue", errno);
    if ((efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0)
        return PROBE_FAIL(NAME, "eventfd", errno);
    uint64_t value = 0;
    if (read(efd, &value, sizeof value) != -1 || errno != EAGAIN)
        return PROBE_FAIL(NAME, "empty eventfd read is not EAGAIN", EINVAL);
    struct kevent change, event;
    EV_SET(&change, efd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &change, 1, NULL, 0, NULL))
        return PROBE_FAIL(NAME, "kevent add eventfd", errno);
    pthread_t thread;
    int error = pthread_create(&thread, NULL, writer, NULL);
    if (error)
        return PROBE_FAIL(NAME, "pthread_create", error);
    struct timespec timeout = {2, 0};
    int got = kevent(kq, NULL, 0, &event, 1, &timeout);
    void *thread_result = NULL;
    pthread_join(thread, &thread_result);
    if (got != 1 || (int)event.ident != efd || thread_result)
        return PROBE_FAIL(NAME, "eventfd did not wake kqueue", got < 0 ? errno : ETIMEDOUT);
    if (read(efd, &value, sizeof value) != sizeof value || value != 1)
        return PROBE_FAIL(NAME, "eventfd value", EINVAL);
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    if (kevent(kq, NULL, 0, &event, 1, &timeout) != 0)
        return PROBE_FAIL(NAME, "eventfd still readable after drain", EINVAL);
    /* Two writes add up; one read returns the sum. */
    uint64_t two = 2, three = 3;
    if (write(efd, &two, 8) != 8 || write(efd, &three, 8) != 8 ||
        read(efd, &value, 8) != 8 || value != 5)
        return PROBE_FAIL(NAME, "eventfd counter", EINVAL);

    EV_SET(&change, efd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(kq, &change, 1, NULL, 0, NULL);
    EV_SET(&change, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);
    double start = now_ms();
    if (kevent(kq, &change, 1, NULL, 0, NULL))
        return PROBE_FAIL(NAME, "kevent add timer", errno);
    timeout.tv_sec = 2;
    got = kevent(kq, NULL, 0, &event, 1, &timeout);
    double elapsed = now_ms() - start;
    if (got != 1 || event.filter != EVFILT_TIMER)
        return PROBE_FAIL(NAME, "timer event", got < 0 ? errno : ETIMEDOUT);
    PROBE_LOG(NAME, "timer fired after %.1f ms", elapsed);
    if (elapsed < 45 || elapsed > 1000)
        return PROBE_FAIL(NAME, "timer duration", EINVAL);
    close(efd);
    close(kq);
    PROBE_LOG(NAME, "eventfd woke kqueue from a thread, counter adds, 50 ms timer ok");
    return PROBE_PASS(NAME);
}
