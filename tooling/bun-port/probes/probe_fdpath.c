/* PS5Term - Bun port probe: descriptor → path. The PS5 kernel has neither FreeBSD 13's
 * fcntl(F_KINFO) (EINVAL) nor FreeBSD 11's sysctl kern.proc.filedesc (ENOENT), so ps5compat
 * remembers the path each descriptor was opened with (fdpath.c); this checks that layer.
 * GPL-3.0-or-later. */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>

#include "probe.h"

long ps5compat_fd_path(int fd, char *buf, size_t cap);

#define DIR "/data/ps5term-dev/fdp"

static int expect(const char *what, int fd, const char *want)
{
    char buf[PATH_MAX];
    errno = 0;
    long n = ps5compat_fd_path(fd, buf, sizeof buf);
    int ok = want ? n > 0 && strcmp(buf, want) == 0 : n < 0 && errno == ENOENT;
    PROBE_LOG("probe_fdpath", "%s: fd %d -> %ld \"%s\" errno=%d %s", what, fd, n, n > 0 ? buf : "", n < 0 ? errno : 0,
              ok ? "ok" : "WRONG");
    return ok;
}

int main(void)
{
    PROBE_START("probe_fdpath");
    int ok = 1;
    mkdir(DIR, 0755);
    int dir = open(DIR, O_RDONLY | O_DIRECTORY);
    ok &= expect("open dir", dir, DIR);

    mkdirat(dir, "tmp", 0755); /* bun install: extract, rename into the cache, open */
    renameat(dir, "tmp", dir, "pkg");
    int pkg = openat(dir, "./pkg/../pkg", O_RDONLY | O_DIRECTORY);
    ok &= expect("openat relative", pkg, DIR "/pkg");

    int copy = fcntl(pkg, F_DUPFD_CLOEXEC, 0);
    ok &= expect("F_DUPFD_CLOEXEC", copy, DIR "/pkg");
    int copy2 = dup(dir);
    ok &= expect("dup", copy2, DIR);

    int file = openat(dir, "f.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ok &= expect("created file", file, DIR "/f.txt");
    renameat(dir, "f.txt", dir, "g.txt");
    ok &= expect("renamed after open", file, NULL);

    close(file);
    close(copy);
    close(copy2);
    close(pkg);
    unlinkat(dir, "g.txt", 0);
    unlinkat(dir, "pkg", AT_REMOVEDIR);
    close(dir);
    rmdir(DIR);
    errno = 0;
    long closed = ps5compat_fd_path(pkg, (char[8]){0}, 8);
    ok &= closed < 0 && errno == EBADF;
    PROBE_LOG("probe_fdpath", "closed fd -> %ld errno=%d", closed, errno);
    return ok ? PROBE_PASS("probe_fdpath") : PROBE_FAIL("probe_fdpath", "wrong path", EINVAL);
}
