/* PS5Term - Bun port feasibility probes: shared reporting. GPL-3.0-or-later.
 * Every probe announces "[name] PID <pid>" and ends with "PASS name" or "FAIL name <errno>"
 * in the kernel log (klogsrv, port 3232), which tools/dev/ps5_bun_console.py waits for.
 * Arguments come one per line from /data/ps5term-dev/args. */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/klog.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_LOG(name, ...)                                                                       \
    do                                                                                             \
    {                                                                                              \
        char probe_line_[512];                                                                     \
        snprintf(probe_line_, sizeof probe_line_, __VA_ARGS__);                                    \
        klog_printf("[%s] %s\n", name, probe_line_);                                               \
    } while (0)

#define PROBE_START(name) klog_printf("[%s] PID %d\n", name, (int)getpid())
#define PROBE_PASS(name) (klog_printf("PASS %s\n", name), 0)
#define PROBE_FAIL(name, what, error)                                                              \
    (klog_printf("[%s] %s: %s\n", name, what, strerror(error)),                                    \
     klog_printf("FAIL %s %d\n", name, error), 1)

/* Reads /data/ps5term-dev/args; returns the number of arguments (at most max). */
static inline int probe_args(char *storage, size_t size, char **args, int max)
{
    int fd = open("/data/ps5term-dev/args", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t got = read(fd, storage, size - 1);
    close(fd);
    if (got <= 0)
        return 0;
    storage[got] = '\0';
    int count = 0;
    for (char *line = storage; *line && count < max;)
    {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        args[count++] = line;
        if (!end)
            break;
        line = end + 1;
    }
    return count;
}

#ifdef __cplusplus
}
#endif
