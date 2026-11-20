/* PS5Term - Bun port probe: the *at calls the PS5 kernel lacks.
 * Measured without ps5compat: faccessat and readlinkat always give ENOSYS, utimensat EACCES,
 * futimens ESRCH, eaccess ENOSYS; the payload runs with real uid 1 and effective uid 0.
 * ps5compat/fdpath.c implements them on access/fstatat/readlink/utimes/futimes; this checks it.
 * /data (nullfs) ignores atime and /system (exfatfs) has no MNT_RDONLY flag, also natively.
 * GPL-3.0-or-later. */
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "probe.h"

#define DIR "/data/ps5term-dev/fax"

static int ok = 1;

static void expect(const char *what, int rc, int want_errno)
{
    int e = rc < 0 ? errno : 0;
    int good = want_errno ? rc == -1 && e == want_errno : rc >= 0;
    ok &= good;
    PROBE_LOG("probe_faccessat", "%s: %d errno=%d %s", what, rc, e, good ? "ok" : "WRONG");
    errno = 0;
}

int main(void)
{
    PROBE_START("probe_faccessat");
    mkdir(DIR, 0755);
    mkdir(DIR "/sub", 0755);
    int fd = open(DIR "/sub/file", O_RDWR | O_CREAT, 0644);
    int dir = open(DIR, O_RDONLY | O_DIRECTORY);
    errno = 0;

    expect("faccessat F_OK", faccessat(dir, "sub/file", F_OK, 0), 0);
    expect("faccessat F_OK missing", faccessat(dir, "sub/missing", F_OK, 0), ENOENT);
    expect("faccessat R_OK (real uid)", faccessat(dir, "sub/file", R_OK, 0), 0);
    expect("faccessat W_OK (real uid 1, root file)", faccessat(dir, "sub/file", W_OK, 0), EACCES);
    expect("faccessat W_OK AT_EACCESS", faccessat(dir, "sub/file", W_OK, AT_EACCESS), 0);
    expect("faccessat X_OK AT_EACCESS file 0644", faccessat(dir, "sub/file", X_OK, AT_EACCESS), EACCES);
    expect("faccessat X_OK AT_EACCESS dir", faccessat(dir, "sub", X_OK, AT_EACCESS), 0);
    expect("eaccess W_OK", eaccess(DIR "/sub/file", W_OK), 0);
    /* /system is exfatfs mounted without MNT_RDONLY (flags 0x1040), so no EROFS there */

    char lb[64];
    expect("readlinkat regular", (int)readlinkat(dir, "sub/file", lb, sizeof lb), EINVAL);
    expect("readlinkat missing", (int)readlinkat(dir, "nope", lb, sizeof lb), ENOENT);

    struct stat st;
    struct timespec ts[2] = {{1000000000, 0}, {1100000000, 500000000}};
    expect("utimensat", utimensat(dir, "sub/file", ts, 0), 0);
    fstat(fd, &st);
    PROBE_LOG("probe_faccessat", "  atime=%ld mtime=%ld.%09ld", (long)st.st_atime, (long)st.st_mtime,
              (long)st.st_mtim.tv_nsec);
    /* /data (nullfs) ignores atime even for the kernel's own utimes, so only mtime is checked */
    ok &= st.st_mtime == 1100000000 && st.st_mtim.tv_nsec == 500000000;

    struct timespec omit[2] = {{0, UTIME_OMIT}, {1200000000, 0}};
    expect("futimens (atime omitted)", futimens(fd, omit), 0);
    fstat(fd, &st);
    PROBE_LOG("probe_faccessat", "  atime=%ld mtime=%ld", (long)st.st_atime, (long)st.st_mtime);
    ok &= st.st_mtime == 1200000000;

    expect("futimens NULL (now)", futimens(fd, NULL), 0);
    fstat(fd, &st);
    ok &= st.st_mtime > 1700000000;
    expect("utimensat missing", utimensat(dir, "nope", ts, 0), ENOENT);
    struct timespec bad[2] = {{0, 2000000000}, {0, 0}};
    expect("futimens bad nsec", futimens(fd, bad), EINVAL);

    close(fd);
    close(dir);
    unlink(DIR "/sub/file");
    rmdir(DIR "/sub");
    rmdir(DIR);
    return ok ? PROBE_PASS("probe_faccessat") : PROBE_FAIL("probe_faccessat", "wrong result", EINVAL);
}
