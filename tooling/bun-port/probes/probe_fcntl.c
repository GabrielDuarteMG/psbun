/* PS5Term - Bun port probe: which fcntl commands the payload kernel accepts on
 * sockets, pipes and files. GPL-3.0-or-later. Reports only; always PASS. */
#include <netinet/in.h>
#include <sys/socket.h>

#include "probe.h"

static void try_fd(const char *what, int fd)
{
    int r;
    errno = 0; r = fcntl(fd, F_GETFL);
    PROBE_LOG("probe_fcntl", "%s GETFL=%d errno=%d", what, r, errno);
    errno = 0; r = fcntl(fd, F_SETFL, (r < 0 ? 0 : r) | O_NONBLOCK);
    PROBE_LOG("probe_fcntl", "%s SETFL=%d errno=%d", what, r, errno);
    errno = 0; r = fcntl(fd, F_GETFD);
    PROBE_LOG("probe_fcntl", "%s GETFD=%d errno=%d", what, r, errno);
    errno = 0; r = fcntl(fd, F_SETFD, FD_CLOEXEC);
    PROBE_LOG("probe_fcntl", "%s SETFD=%d errno=%d", what, r, errno);
    errno = 0; r = fcntl(fd, F_DUPFD, 0);
    PROBE_LOG("probe_fcntl", "%s DUPFD=%d errno=%d", what, r, errno);
    if (r >= 0) close(r);
    errno = 0; r = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    PROBE_LOG("probe_fcntl", "%s DUPFD_CLOEXEC=%d errno=%d", what, r, errno);
    if (r >= 0) close(r);
    errno = 0; r = dup(fd);
    PROBE_LOG("probe_fcntl", "%s dup=%d errno=%d", what, r, errno);
    if (r >= 0) close(r);
}

int main(void)
{
    PROBE_START("probe_fcntl");
    try_fd("fd0", 0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    try_fd("tcp", s);
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0)
        try_fd("unixpair", sp[0]);
    int p[2];
    if (pipe(p) == 0)
        try_fd("pipe", p[0]);
    int f = open("/data/ps5term-dev/args", O_RDONLY);
    try_fd("file", f);
    return PROBE_PASS("probe_fcntl");
}
