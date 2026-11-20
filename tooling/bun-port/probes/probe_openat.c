/* PS5Term - Bun port probe: does the SDK's openat()/open() report a missing file as -1/ENOENT?
 * Bun opens files with openat(AT_FDCWD, ...); a stub that returns the kernel's raw errno would hand
 * back ENOENT (2) as if it were a descriptor. GPL-3.0-or-later. */
#include <sys/types.h>
#include <fcntl.h>

#include "probe.h"

int main(void)
{
    PROBE_START("probe_openat");
    const char *missing = "/etc/ssl/cert.pem";
    errno = 0;
    int at = openat(AT_FDCWD, missing, O_RDONLY | O_CLOEXEC);
    int at_errno = errno;
    errno = 0;
    int plain = open(missing, O_RDONLY | O_CLOEXEC);
    int plain_errno = errno;
    errno = 0;
    int existing = openat(AT_FDCWD, "/data/ps5term-dev/args", O_RDONLY | O_CLOEXEC);
    int existing_errno = errno;
    PROBE_LOG("probe_openat", "openat(missing)=%d errno=%d open(missing)=%d errno=%d openat(args)=%d errno=%d",
              at, at_errno, plain, plain_errno, existing, existing_errno);
    if (existing >= 0)
        close(existing);
    return at == -1 && at_errno == ENOENT && plain == -1 && existing > 2
               ? PROBE_PASS("probe_openat")
               : PROBE_FAIL("probe_openat", "openat result", at_errno ? at_errno : EINVAL);
}
