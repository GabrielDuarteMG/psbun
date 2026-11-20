/* PS5Term - Bun port probe: non-blocking TCP connect completion seen by kqueue
 * (EVFILT_WRITE), as usockets waits for it. Argument: IPv4 address, port. GPL-3.0-or-later. */
#include <sys/types.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <time.h>

#include "probe.h"

int main(void)
{
    PROBE_START("probe_connect");
    char storage[256], *args[2];
    if (probe_args(storage, sizeof storage, args, 2) != 2)
        return PROBE_FAIL("probe_connect", "args", EINVAL);
    struct sockaddr_in addr = {.sin_len = sizeof addr, .sin_family = AF_INET,
                               .sin_port = htons((uint16_t)atoi(args[1]))};
    inet_pton(AF_INET, args[0], &addr.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int flags = fcntl(fd, F_GETFL);
    PROBE_LOG("probe_connect", "nonblock %d", fcntl(fd, F_SETFL, flags | O_NONBLOCK));
    int r = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    PROBE_LOG("probe_connect", "connect=%d errno=%d", r, r ? errno : 0);
    int kq = kqueue();
    struct kevent change[2], events[2];
    EV_SET(&change[0], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    EV_SET(&change[1], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    PROBE_LOG("probe_connect", "kevent add=%d errno=%d", kevent(kq, change, 2, NULL, 0, NULL), errno);
    struct timespec wait = {10, 0};
    int n = kevent(kq, NULL, 0, events, 2, &wait);
    PROBE_LOG("probe_connect", "kevent wait=%d", n);
    for (int i = 0; i < n; i++)
        PROBE_LOG("probe_connect", "event filter=%d flags=%#x fflags=%u data=%ld", events[i].filter,
                  events[i].flags, events[i].fflags, (long)events[i].data);
    int error = -1;
    socklen_t length = sizeof error;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
    PROBE_LOG("probe_connect", "SO_ERROR=%d", error);
    return n > 0 && error == 0 ? PROBE_PASS("probe_connect") : PROBE_FAIL("probe_connect", "connect", n);
}
