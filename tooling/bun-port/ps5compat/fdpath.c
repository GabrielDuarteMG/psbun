/* PS5Term - Bun port: descriptor → path for the PS5 kernel, and the path-based calls built on it.
 *
 * Bun asks the kernel for the path of an open descriptor (bun install's cache, the module resolver,
 * globbing, the bundler). FreeBSD 13+ answers fcntl(F_KINFO); FreeBSD 11 answers sysctl
 * kern.proc.filedesc. The PS5 kernel answers neither: F_KINFO fails with EINVAL and the sysctl node
 * does not exist (ENOENT; console probe_fdpath).
 *
 * So remember the path each descriptor was opened with (open, openat, dup, dup2, fcntl F_DUPFD*)
 * and forget it on close. A lookup resolves the remembered path with realpath() and returns it only
 * if it still names the same file as the descriptor (st_dev and st_ino of fstat and stat agree); a
 * file renamed or removed since it was opened gives ENOENT, never another file's path.
 *
 * The kernel also lacks working faccessat, readlinkat (both ENOSYS), utimensat (EACCES),
 * futimens (ESRCH) and eaccess (ENOSYS) (console probe_faccessat). They are implemented here with
 * fstatat, access, readlink, utimes, lutimes and futimes on the directory descriptor's path; the
 * broken calls are never made.
 * GPL-3.0-or-later. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
int __real_dup(int fd);
int __real_dup2(int fd, int to);
int ps5compat_checked_openat(int dirfd, const char *path, int flags, ...);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char **paths;  /* indexed by descriptor; NULL when unknown */
static size_t npaths;

static void set_locked(int fd, char *path)
{
    if (fd < 0)
    {
        free(path);
        return;
    }
    if ((size_t)fd >= npaths)
    {
        if (!path)
            return;
        size_t n = npaths ? npaths : 256;
        while (n <= (size_t)fd)
            n *= 2;
        char **grown = realloc(paths, n * sizeof *paths);
        if (!grown)
        {
            free(path);
            return;
        }
        memset(grown + npaths, 0, (n - npaths) * sizeof *paths);
        paths = grown;
        npaths = n;
    }
    free(paths[fd]);
    paths[fd] = path;
}

void ps5compat_fdpath_forget(int fd)
{
    pthread_mutex_lock(&lock);
    set_locked(fd, NULL);
    pthread_mutex_unlock(&lock);
}

void ps5compat_fdpath_copy(int from, int to)
{
    if (from == to || to < 0)
        return;
    pthread_mutex_lock(&lock);
    char *path = from >= 0 && (size_t)from < npaths && paths[from] ? strdup(paths[from]) : NULL;
    set_locked(to, path);
    pthread_mutex_unlock(&lock);
}

/* Absolute form of `path` opened relative to `dirfd`; NULL when the base is unknown. */
static void remember(int fd, int dirfd, const char *path)
{
    if (fd < 0 || !path || !*path)
        return;
    char base[PATH_MAX];
    base[0] = '\0';
    if (path[0] != '/')
    {
        if (dirfd == AT_FDCWD)
        {
            if (!getcwd(base, sizeof base) || base[0] != '/')
                base[0] = '\0';
        }
        else
        {
            pthread_mutex_lock(&lock);
            if (dirfd >= 0 && (size_t)dirfd < npaths && paths[dirfd])
                strlcpy(base, paths[dirfd], sizeof base);
            pthread_mutex_unlock(&lock);
        }
        if (!base[0])
        {
            ps5compat_fdpath_forget(fd);
            return;
        }
    }
    size_t blen = strlen(base), plen = strlen(path);
    char *full = malloc(blen + 1 + plen + 1);
    if (!full)
    {
        ps5compat_fdpath_forget(fd);
        return;
    }
    memcpy(full, base, blen);
    size_t at = blen;
    if (blen)
        full[at++] = '/';
    memcpy(full + at, path, plen + 1);
    pthread_mutex_lock(&lock);
    set_locked(fd, full);
    pthread_mutex_unlock(&lock);
}

int __wrap_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT)
    {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int fd = __real_open(path, flags, mode);
    remember(fd, AT_FDCWD, path);
    return fd;
}

int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT)
    {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int fd = ps5compat_checked_openat(dirfd, path, flags, mode);
    remember(fd, dirfd, path);
    return fd;
}

int __wrap_dup(int fd)
{
    int copy = __real_dup(fd);
    ps5compat_fdpath_copy(fd, copy);
    return copy;
}

int __wrap_dup2(int fd, int to)
{
    int result = __real_dup2(fd, to);
    if (result >= 0)
        ps5compat_fdpath_copy(fd, result);
    return result;
}

/* Writes the path of `fd` into `buf` (NUL-terminated) and returns its length, or -1 with errno:
 * EBADF for a closed descriptor, ENOENT when no current name is known, ENAMETOOLONG. */
long ps5compat_fd_path(int fd, char *buf, size_t cap)
{
    struct stat fst, pst;
    if (fstat(fd, &fst) != 0)
        return -1;
    char known[PATH_MAX];
    known[0] = '\0';
    pthread_mutex_lock(&lock);
    if (fd >= 0 && (size_t)fd < npaths && paths[fd])
        strlcpy(known, paths[fd], sizeof known);
    pthread_mutex_unlock(&lock);
    char resolved[PATH_MAX];
    if (!known[0] || !realpath(known, resolved) || stat(resolved, &pst) != 0 || pst.st_dev != fst.st_dev ||
        pst.st_ino != fst.st_ino)
    {
        errno = ENOENT;
        return -1;
    }
    size_t len = strlen(resolved);
    if (len + 1 > cap)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, resolved, len + 1);
    return (long)len;
}

/* `path` relative to `dirfd` as a path the non-*at calls accept. */
static int at_path(int dirfd, const char *path, char *out, size_t cap)
{
    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        if (strlcpy(out, path, cap) >= cap)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    long n = ps5compat_fd_path(dirfd, out, cap);
    if (n < 0)
        return -1;
    if (!*path)
        return 0;
    if ((size_t)n + 1 + strlen(path) + 1 > cap)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[n] = '/';
    strcpy(out + n + 1, path);
    return 0;
}

/* The kernel's permission rule (vaccess) with the effective ids, plus EROFS for writes to a
 * read-only mount. */
static int effective_access(const char *path, int mode, int nofollow)
{
    struct stat st;
    if ((nofollow ? lstat(path, &st) : stat(path, &st)) != 0)
        return -1;
    if (mode == F_OK)
        return 0;
    if (mode & W_OK)
    {
        struct statfs fs;
        if (statfs(path, &fs) == 0 && (fs.f_flags & MNT_RDONLY))
        {
            errno = EROFS;
            return -1;
        }
    }
    uid_t uid = geteuid();
    mode_t granted;
    if (uid == 0)
    {
        /* root may read and write anything; execute needs one execute bit, except on directories */
        granted = S_IRWXO;
        if (!S_ISDIR(st.st_mode) && !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            granted &= ~S_IXOTH;
    }
    else if (st.st_uid == uid)
        granted = (st.st_mode & S_IRWXU) >> 6;
    else
    {
        int member = st.st_gid == getegid();
        gid_t groups[NGROUPS_MAX];
        int n = member ? 0 : getgroups(NGROUPS_MAX, groups);
        for (int i = 0; i < n && !member; i++)
            member = groups[i] == st.st_gid;
        granted = member ? (st.st_mode & S_IRWXG) >> 3 : st.st_mode & S_IRWXO;
    }
    mode_t want = (mode & R_OK ? S_IROTH : 0) | (mode & W_OK ? S_IWOTH : 0) | (mode & X_OK ? S_IXOTH : 0);
    if ((granted & want) != want)
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int __wrap_eaccess(const char *path, int mode)
{
    return effective_access(path, mode, 0);
}

int __wrap_faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW))
    {
        errno = EINVAL;
        return -1;
    }
    if (mode == F_OK)
    {
        struct stat st;
        return fstatat(dirfd, path, &st, flags & AT_SYMLINK_NOFOLLOW);
    }
    char full[PATH_MAX];
    if (at_path(dirfd, path, full, sizeof full) != 0)
        return -1;
    if (flags)
        return effective_access(full, mode, flags & AT_SYMLINK_NOFOLLOW);
    return access(full, mode);
}

ssize_t __wrap_readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
    char full[PATH_MAX];
    if (at_path(dirfd, path, full, sizeof full) != 0)
        return -1;
    return readlink(full, buf, size);
}

/* timespec pair with UTIME_NOW/UTIME_OMIT → timeval pair (microseconds); `st` gives the
 * current times for UTIME_OMIT. */
static void to_timevals(const struct timespec times[2], const struct stat *st, struct timeval tv[2])
{
    struct timeval now;
    gettimeofday(&now, NULL);
    const struct timespec current[2] = {st->st_atim, st->st_mtim};
    for (int i = 0; i < 2; i++)
    {
        const struct timespec *t = !times || times[i].tv_nsec == UTIME_NOW ? NULL
                                   : times[i].tv_nsec == UTIME_OMIT        ? &current[i]
                                                                           : &times[i];
        if (!t)
            tv[i] = now;
        else
        {
            tv[i].tv_sec = t->tv_sec;
            tv[i].tv_usec = t->tv_nsec / 1000;
        }
    }
}

static int bad_times(const struct timespec times[2])
{
    for (int i = 0; times && i < 2; i++)
        if (times[i].tv_nsec != UTIME_NOW && times[i].tv_nsec != UTIME_OMIT &&
            (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000L))
        {
            errno = EINVAL;
            return 1;
        }
    return 0;
}

int __wrap_futimens(int fd, const struct timespec times[2])
{
    struct stat st;
    if (bad_times(times) || fstat(fd, &st) != 0)
        return -1;
    if (times && times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT)
        return 0;
    struct timeval tv[2];
    to_timevals(times, &st, tv);
    return futimes(fd, tv);
}

int __wrap_utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    if (flags & ~AT_SYMLINK_NOFOLLOW)
    {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (bad_times(times) || fstatat(dirfd, path, &st, flags) != 0)
        return -1;
    if (times && times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT)
        return 0;
    char full[PATH_MAX];
    if (at_path(dirfd, path, full, sizeof full) != 0)
        return -1;
    struct timeval tv[2];
    to_timevals(times, &st, tv);
    return flags & AT_SYMLINK_NOFOLLOW ? lutimes(full, tv) : utimes(full, tv);
}
