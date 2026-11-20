/* PS5Term - Bun port probe: which getdents() buffer sizes each file system accepts. Bun reads
 * directories with getdents(fd, buf, 8192); on /data that fails with EINVAL while / works.
 * GPL-3.0-or-later. */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>

#include "probe.h"

static int try_dir(const char *path)
{
    static const size_t sizes[] = {512, 1024, 4096, 8192, 16384, 32768, 65536, 131072};
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
    {
        PROBE_LOG("probe_getdents", "%s: open errno=%d", path, errno);
        return 0;
    }
    struct stat st;
    fstat(fd, &st);
    PROBE_LOG("probe_getdents", "%s: st_blksize=%ld", path, (long)st.st_blksize);
    char *buf = aligned_alloc(16384, 131072);
    int any = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
    {
        lseek(fd, 0, SEEK_SET);
        errno = 0;
        ssize_t n = getdents(fd, buf, sizes[i]);
        PROBE_LOG("probe_getdents", "%s: getdents(%zu)=%zd errno=%d", path, sizes[i], n, n < 0 ? errno : 0);
        any |= n > 0;
    }
    free(buf);
    close(fd);
    return any;
}

int main(void)
{
    PROBE_START("probe_getdents");
    int root = try_dir("/");
    int data = try_dir("/data/ps5term-dev");
    return root && data ? PROBE_PASS("probe_getdents") : PROBE_FAIL("probe_getdents", "no size worked", EINVAL);
}
