/* PS5Term - ps5compat Bun bin shims. GPL-3.0-or-later.
 *
 * /data on the PS5 is a nullfs where symlink() and link() fail with EOPNOTSUPP, so Bun writes
 * a bin shim where it would create a bin symlink, and posix_spawn (spawn.c) runs the shim's
 * target in bun.elf. The file is 0755 and holds exactly three lines:
 *   #!/usr/bin/env bun
 *   // bun-ps5-bin-shim <target>
 *   require(require("path").resolve(__dirname, "<target>"));
 * <target> is the symlink text Bun would have used (relative to the shim's directory, or
 * absolute): 1 to 1023 bytes, none of '"', '\\', '\n', '\r', NUL. Line 3 only serves
 * "bun ./node_modules/.bin/x"; spawn.c never executes it. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps5compat.h"

#define SHIM_HEADER "#!/usr/bin/env bun\n// bun-ps5-bin-shim "
#define SHIM_TARGET_MAX 1023
#define SHIM_READ_MAX 2048

static int valid_target(const char *target, size_t length)
{
    if (length < 1 || length > SHIM_TARGET_MAX)
        return 0;
    for (size_t i = 0; i < length; i++)
        if (target[i] == '"' || target[i] == '\\' || target[i] == '\n' || target[i] == '\r' ||
            target[i] == '\0')
            return 0;
    return 1;
}

ssize_t ps5compat_bin_shim_parse(const void *bytes, size_t size, const char **target)
{
    const size_t header = sizeof SHIM_HEADER - 1;
    if (!bytes || size <= header || memcmp(bytes, SHIM_HEADER, header))
        return -1;
    const char *start = (const char *)bytes + header;
    size_t rest = size - header;
    const char *end = memchr(start, '\n', rest < SHIM_TARGET_MAX + 1 ? rest : SHIM_TARGET_MAX + 1);
    if (!end || !valid_target(start, (size_t)(end - start)))
        return -1;
    *target = start;
    return end - start;
}

static int write_all(int fd, const char *data, size_t size)
{
    while (size)
    {
        ssize_t n = write(fd, data, size);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
        {
            errno = EIO;
            return -1;
        }
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

int ps5compat_bin_shim_create(const char *path, const char *target)
{
    size_t length = target ? strnlen(target, SHIM_TARGET_MAX + 1) : 0;
    if (!path || !valid_target(target, length))
    {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
    if (fd < 0)
        return -1;
    static const char line3[] = "\nrequire(require(\"path\").resolve(__dirname, \"";
    static const char tail[] = "\"));\n";
    if (write_all(fd, SHIM_HEADER, sizeof SHIM_HEADER - 1) || write_all(fd, target, length) ||
        write_all(fd, line3, sizeof line3 - 1) || write_all(fd, target, length) ||
        write_all(fd, tail, sizeof tail - 1) || fchmod(fd, 0755))
    {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    if (close(fd))
    {
        int saved = errno;
        unlink(path);
        errno = saved;
        return -1;
    }
    return 0;
}

ssize_t ps5compat_bin_shim_read(int dirfd, const char *path, char *buf, size_t size)
{
    int fd = openat(dirfd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st))
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    char bytes[SHIM_READ_MAX];
    size_t used = 0;
    while (used < sizeof bytes)
    {
        ssize_t n = read(fd, bytes + used, sizeof bytes - used);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    close(fd);
    const char *target;
    ssize_t length = ps5compat_bin_shim_parse(bytes, used, &target);
    if (length < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)length >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, target, (size_t)length);
    buf[length] = '\0';
    return length;
}

int ps5compat_bin_shim_resolve(const char *shim_path, const char *target, size_t target_len,
                               char out[PATH_MAX])
{
    char joined[PATH_MAX];
    size_t used = 0;
    if (target_len && target[0] != '/')
    {
        const char *slash = strrchr(shim_path, '/');
        const char *dir = slash ? shim_path : ".";
        size_t dir_len = slash ? (size_t)(slash - shim_path) : 1;
        if (slash && dir_len == 0)
            dir_len = 1; /* shim in "/": keep the root */
        if (dir_len + 1 >= sizeof joined)
            return ENAMETOOLONG;
        memcpy(joined, dir, dir_len);
        used = dir_len;
        if (joined[used - 1] != '/')
            joined[used++] = '/';
    }
    if (used + target_len >= sizeof joined)
        return ENAMETOOLONG;
    memcpy(joined + used, target, target_len);
    joined[used + target_len] = '\0';
    if (!realpath(joined, out))
        return errno;
    return 0;
}
