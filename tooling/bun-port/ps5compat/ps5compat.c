/* PS5Term - ps5compat: FreeBSD functions missing from the PS5 payload SDK. GPL-3.0-or-later.
 * See ps5compat.h. Functions the SDK already defines are replaced through -Wl,--wrap (the
 * __wrap_* below); the list is ps5compat.ldflags. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <elf.h>
#include <ps5/klog.h>

#include <netdb.h>
#include <netinet/in.h>
#include "ps5compat.h"

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1 << 2)
#endif

ssize_t __real_read(int fd, void *buf, size_t size);
ssize_t __real_write(int fd, const void *buf, size_t size);
int __real_close(int fd);
int __real_isatty(int fd);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_dlopen(const char *path, int mode);
void *__real_dlsym(void *handle, const char *name);
int __real_dlclose(void *handle);
char *__real_dlerror(void);
int __real_fcntl(int fd, int cmd, ...);
void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int __real_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result);
void ps5compat_fdpath_forget(int fd);  /* fdpath.c */
void ps5compat_fdpath_copy(int from, int to);

/* ---- SDK system call stubs without an error path (syscall_errno.S) --------------------- */

/* Tail of every checked stub: the kernel's error number becomes errno and the result -1
 * (long, so calls returning ssize_t see -1 too). */
long ps5compat_fail_errno(int error)
{
    errno = error;
    return -1;
}

/* ---- getaddrinfo ---------------------------------------------------------------------- */

/* The console's resolver answers EAI_SERVICE for a numeric service without a socket type
 * (Bun's dns.lookup passes "0"), though it resolves the host itself. A numeric service is
 * only a port: resolve without it and store the port in every result. */
int __wrap_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result)
{
    if (!service || !host || !*service || strspn(service, "0123456789") != strlen(service))
        return __real_getaddrinfo(host, service, hints, result);
    unsigned long port = strtoul(service, NULL, 10);
    if (port > 65535)
        return __real_getaddrinfo(host, service, hints, result);
    struct addrinfo copy;
    if (hints)
    {
        copy = *hints;
        copy.ai_flags &= ~AI_NUMERICSERV;
        hints = &copy;
    }
    int status = __real_getaddrinfo(host, NULL, hints, result);
    if (status)
        return status;
    for (struct addrinfo *entry = *result; entry; entry = entry->ai_next)
    {
        if (entry->ai_family == AF_INET)
            ((struct sockaddr_in *)entry->ai_addr)->sin_port = htons((uint16_t)port);
        else if (entry->ai_family == AF_INET6)
            ((struct sockaddr_in6 *)entry->ai_addr)->sin6_port = htons((uint16_t)port);
    }
    return 0;
}

/* ---- mmap ----------------------------------------------------------------------------- */

/* Unchanged mmap; a failure is reported in the kernel log (klogsrv), where allocators that
 * assert on a failed mapping (libpas, mimalloc) leave no other trace of the errno. */
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *result = __real_mmap(addr, length, prot, flags, fd, offset);
    if (result == MAP_FAILED)
    {
        int saved = errno;
        klog_printf("ps5compat: mmap(%p, %zu, prot=%#x, flags=%#x, fd=%d) pid %d: errno %d\n",
                    addr, length, prot, flags, fd, (int)getpid(), saved);
        errno = saved;
    }
    return result;
}

/* ---- fcntl ---------------------------------------------------------------------------- */

/* The payload kernel refuses F_DUPFD_CLOEXEC with EOPNOTSUPP on every descriptor kind
 * (console probe_fcntl), while F_DUPFD and F_SETFD work: compose the two. */
int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (cmd != F_DUPFD_CLOEXEC)
    {
        int result = __real_fcntl(fd, cmd, arg);
        if (cmd == F_DUPFD && result >= 0)
            ps5compat_fdpath_copy(fd, result);
        return result;
    }
    int dup = __real_fcntl(fd, F_DUPFD, arg);
    if (dup >= 0 && __real_fcntl(dup, F_SETFD, FD_CLOEXEC) < 0)
    {
        int saved = errno;
        __real_close(dup);
        errno = saved;
        return -1;
    }
    ps5compat_fdpath_copy(fd, dup);
    return dup;
}

/* ---- eventfd -------------------------------------------------------------------------- */

#define EFD_MAX 4096

typedef struct
{
    pthread_mutex_t lock;
    int peer;      /* Write end of the pipe; the eventfd itself is the read end. */
    int nonblock;  /* EFD_NONBLOCK as requested; the pipe itself is always non-blocking. */
    int semaphore;
    uint64_t count; /* The read end holds one byte exactly while count > 0. */
} EventFd;

static _Atomic(EventFd *) eventfds[EFD_MAX];

static EventFd *eventfd_of(int fd)
{
    if ((unsigned)fd >= EFD_MAX)
        return NULL;
    return atomic_load_explicit(&eventfds[fd], memory_order_acquire);
}

int eventfd(unsigned int initval, int flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
    {
        errno = EINVAL;
        return -1;
    }
    int fds[2];
    if (pipe(fds))
        return -1;
    EventFd *efd = calloc(1, sizeof *efd);
    if (!efd || (unsigned)fds[0] >= EFD_MAX || fcntl(fds[0], F_SETFL, O_NONBLOCK) ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) ||
        ((flags & EFD_CLOEXEC) &&
         (fcntl(fds[0], F_SETFD, FD_CLOEXEC) || fcntl(fds[1], F_SETFD, FD_CLOEXEC))))
    {
        int saved = efd ? errno : ENOMEM;
        free(efd);
        __real_close(fds[0]);
        __real_close(fds[1]);
        errno = (unsigned)fds[0] >= EFD_MAX ? EMFILE : saved;
        return -1;
    }
    pthread_mutex_init(&efd->lock, NULL);
    efd->peer = fds[1];
    efd->nonblock = (flags & EFD_NONBLOCK) != 0;
    efd->semaphore = (flags & EFD_SEMAPHORE) != 0;
    efd->count = initval;
    if (initval)
        __real_write(fds[1], "", 1);
    atomic_store_explicit(&eventfds[fds[0]], efd, memory_order_release);
    return fds[0];
}

static ssize_t eventfd_do_write(EventFd *efd, const void *buf, size_t size)
{
    uint64_t value;
    if (size < sizeof value)
    {
        errno = EINVAL;
        return -1;
    }
    memcpy(&value, buf, sizeof value);
    if (value == UINT64_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&efd->lock);
    if (efd->count > UINT64_MAX - 1 - value)
    {
        pthread_mutex_unlock(&efd->lock);
        errno = EAGAIN; /* Would overflow: a blocking eventfd would wait for a reader. */
        return -1;
    }
    if (efd->count == 0 && value)
        __real_write(efd->peer, "", 1);
    efd->count += value;
    pthread_mutex_unlock(&efd->lock);
    return sizeof value;
}

static ssize_t eventfd_do_read(int fd, EventFd *efd, void *buf, size_t size)
{
    uint64_t value;
    if (size < sizeof value)
    {
        errno = EINVAL;
        return -1;
    }
    for (;;)
    {
        pthread_mutex_lock(&efd->lock);
        if (efd->count)
        {
            value = efd->semaphore ? 1 : efd->count;
            efd->count -= value;
            if (!efd->count)
            {
                char drain[16];
                while (__real_read(fd, drain, sizeof drain) > 0)
                    ;
            }
            pthread_mutex_unlock(&efd->lock);
            memcpy(buf, &value, sizeof value);
            return sizeof value;
        }
        pthread_mutex_unlock(&efd->lock);
        if (efd->nonblock)
        {
            errno = EAGAIN;
            return -1;
        }
        struct pollfd wait = {fd, POLLIN, 0};
        if (poll(&wait, 1, -1) < 0 && errno != EINTR)
            return -1;
    }
}

int eventfd_read(int fd, eventfd_t *value)
{
    return read(fd, value, sizeof *value) == sizeof *value ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value)
{
    return write(fd, &value, sizeof value) == sizeof value ? 0 : -1;
}

ssize_t __wrap_read(int fd, void *buf, size_t size)
{
    EventFd *efd = eventfd_of(fd);
    return efd ? eventfd_do_read(fd, efd, buf, size) : __real_read(fd, buf, size);
}

ssize_t __wrap_write(int fd, const void *buf, size_t size)
{
    EventFd *efd = eventfd_of(fd);
    return efd ? eventfd_do_write(efd, buf, size) : __real_write(fd, buf, size);
}

int __wrap_close(int fd)
{
    if ((unsigned)fd < EFD_MAX)
    {
        EventFd *efd = atomic_exchange_explicit(&eventfds[fd], NULL, memory_order_acq_rel);
        if (efd)
        {
            __real_close(efd->peer);
            pthread_mutex_destroy(&efd->lock);
            free(efd);
        }
    }
    ps5compat_fdpath_forget(fd); /* before the descriptor number can be reused */
    return __real_close(fd);
}

/* ---- auxv, entropy, copy_file_range ------------------------------------------------------ */

int elf_aux_info(int aux, void *buf, int buflen)
{
    int value = 0;
    size_t size = sizeof value;
    const char *name = aux == AT_PAGESZ ? "hw.pagesize" : aux == AT_NCPUS ? "hw.ncpu" : NULL;
    if (!name)
        return ENOENT;
    if (!buf || buflen != (int)sizeof value)
        return EINVAL;
    if (sysctlbyname(name, &value, &size, NULL, 0))
        return errno;
    memcpy(buf, &value, sizeof value);
    return 0;
}

static _Atomic int urandom_fd = -1;

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    (void)flags; /* /dev/urandom never blocks once the system is up. */
    int fd = atomic_load(&urandom_fd);
    if (fd < 0)
    {
        int opened = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (opened < 0)
            return -1;
        int expected = -1;
        if (atomic_compare_exchange_strong(&urandom_fd, &expected, opened))
            fd = opened;
        else
        {
            __real_close(opened);
            fd = expected;
        }
    }
    size_t done = 0;
    while (done < buflen)
    {
        ssize_t got = __real_read(fd, (char *)buf + done, buflen - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return done ? (ssize_t)done : -1;
        done += (size_t)got;
    }
    return (ssize_t)done;
}

int getentropy(void *buf, size_t buflen)
{
    if (buflen > 256)
    {
        errno = EIO;
        return -1;
    }
    return getrandom(buf, buflen, 0) == (ssize_t)buflen ? 0 : -1;
}

/* copy_file_range(2) is a FreeBSD 13 system call the PS5 kernel lacks. Bun's FreeBSD code treats
 * it as always present (it falls back to read/write only on EXDEV/EINVAL/EOPNOTSUPP/EBADF), so
 * implement it with the same read/write loop FreeBSD's kernel uses for file systems without a
 * native copy (vn_generic_copy_file_range): explicit offsets are used and advanced instead of the
 * file positions, the return value is the number of bytes copied (0 at end of input). */
ssize_t copy_file_range(int infd, off_t *inoffp, int outfd, off_t *outoffp, size_t len,
                        unsigned int flags)
{
    if (flags != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (infd == outfd && (!inoffp || !outoffp))
    {
        errno = EINVAL;
        return -1;
    }
    enum { CHUNK = 256 * 1024 };
    char *buf = malloc(len < CHUNK ? (len ? len : 1) : CHUNK);
    if (!buf)
        return -1;
    size_t total = 0;
    int error = 0;
    while (total < len)
    {
        size_t want = len - total < CHUNK ? len - total : CHUNK;
        ssize_t got = inoffp ? pread(infd, buf, want, *inoffp) : __real_read(infd, buf, want);
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            error = errno;
            break;
        }
        if (got == 0)
            break;
        ssize_t put = 0;
        while (put < got)
        {
            ssize_t n = outoffp ? pwrite(outfd, buf + put, got - put, *outoffp + put)
                                : __real_write(outfd, buf + put, got - put);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                error = errno;
                break;
            }
            put += n;
        }
        /* bytes read but not written stay unconsumed in the input */
        if (inoffp)
            *inoffp += put;
        else if (put < got)
            lseek(infd, put - got, SEEK_CUR);
        if (outoffp)
            *outoffp += put;
        total += put;
        if (error)
            break;
    }
    free(buf);
    if (error && total == 0)
    {
        errno = error;
        return -1;
    }
    return (ssize_t)total;
}

/* ---- FreeBSD 12-14 libc additions missing from the SDK ----------------------------------- */

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

void *memmem(const void *big, size_t big_len, const void *little, size_t little_len)
{
    if (!little_len)
        return (void *)big;
    const unsigned char *b = big, *l = little;
    for (size_t i = 0; i + little_len <= big_len; i++)
    {
        const unsigned char *hit = memchr(b + i, l[0], big_len - little_len - i + 1);
        if (!hit)
            return NULL;
        i = (size_t)(hit - b);
        if (!memcmp(hit, l, little_len))
            return (void *)hit;
    }
    return NULL;
}

int timingsafe_bcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < size; i++)
        diff |= x[i] ^ y[i];
    return diff != 0;
}

void explicit_bzero(void *buf, size_t size)
{
    memset(buf, 0, size);
    __asm__ __volatile__("" : : "r"(buf) : "memory");
}

/* close_range(2) (FreeBSD 12.2+): closes, or with CLOSE_RANGE_CLOEXEC marks close-on-exec, every
 * open descriptor in [low, high], bounded by the process's descriptor table size. */
int close_range(unsigned int low, unsigned int high, int flags)
{
    if (low > high || (flags & ~CLOSE_RANGE_CLOEXEC))
    {
        errno = EINVAL;
        return -1;
    }
    long table = sysconf(_SC_OPEN_MAX);
    unsigned int last = table > 0 && (unsigned long)table - 1 < high ? (unsigned int)table - 1 : high;
    for (unsigned int fd = low; fd <= last; fd++)
    {
        if (flags & CLOSE_RANGE_CLOEXEC)
            fcntl((int)fd, F_SETFD, FD_CLOEXEC);
        else
            close((int)fd);
        if (fd == UINT_MAX)
            break;
    }
    return 0;
}

int dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd || (flags & ~O_CLOEXEC))
    {
        errno = EINVAL;
        return -1;
    }
    if (dup2(oldfd, newfd) < 0)
        return -1;
    if ((flags & O_CLOEXEC) && fcntl(newfd, F_SETFD, FD_CLOEXEC))
        return -1;
    return newfd;
}

int accept4(int s, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
    {
        errno = EINVAL;
        return -1;
    }
    int fd = accept(s, addr, addrlen);
    if (fd < 0)
        return -1;
    if (((flags & SOCK_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC)) ||
        ((flags & SOCK_NONBLOCK) && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)))
    {
        int saved = errno;
        __real_close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Sony's libkernel names threads with a 32-byte buffer and takes no size argument. */
int __real_pthread_getname_np(pthread_t thread, char *name);

int __wrap_pthread_getname_np(pthread_t thread, char *name, size_t size)
{
    char buffer[64] = "";
    int error = __real_pthread_getname_np(thread, buffer);
    if (error)
        return error;
    if (!size)
        return ERANGE;
    snprintf(name, size, "%s", buffer);
    return 0;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    pthread_set_name_np(thread, name);
    return 0;
}

/* The SDK's libc.a clock_nanosleep is a stub for a FreeBSD 13 system call the PS5 kernel lacks
 * (ENOSYS). Relative sleeps map to nanosleep; absolute ones sleep until the clock reaches the
 * deadline. Returns an error number, like clock_nanosleep. */
int __wrap_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                           struct timespec *remain)
{
    struct timespec now;
    if (!request || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L)
        return EINVAL;
    if (clock_gettime(clock, &now))
        return EINVAL;
    if (!(flags & TIMER_ABSTIME))
        return nanosleep(request, remain) ? errno : 0;
    for (;;)
    {
        struct timespec left = {request->tv_sec - now.tv_sec, request->tv_nsec - now.tv_nsec};
        if (left.tv_nsec < 0)
        {
            left.tv_sec--;
            left.tv_nsec += 1000000000L;
        }
        if (left.tv_sec < 0 || (left.tv_sec == 0 && left.tv_nsec == 0))
            return 0;
        if (nanosleep(&left, NULL))
            return errno;
        if (clock_gettime(clock, &now))
            return EINVAL;
    }
}

/* ---- C++ thread_local destructors -------------------------------------------------------- */

/* libc++abi's fallback keeps its destructor list in emulated TLS, whose own pthread key is
 * destroyed first, so thread_local destructors never ran (probe_cxx). libc++abi calls this
 * weak hook instead when it exists. The key is created before any emulated TLS key, so its
 * destructor runs while thread_local objects are still alive. Destructors of the main
 * thread's thread_local objects are not run at exit. */
typedef struct ThreadAtExit
{
    void (*destructor)(void *);
    void *object;
    struct ThreadAtExit *next;
} ThreadAtExit;

static pthread_key_t thread_atexit_key;

static void run_thread_destructors(void *value)
{
    for (ThreadAtExit *head = value; head; head = pthread_getspecific(thread_atexit_key))
    {
        pthread_setspecific(thread_atexit_key, NULL);
        while (head)
        {
            ThreadAtExit *next = head->next;
            head->destructor(head->object);
            free(head);
            head = next;
        }
    }
}

__attribute__((constructor(101))) static void thread_atexit_init(void)
{
    pthread_key_create(&thread_atexit_key, run_thread_destructors);
}

int __cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso)
{
    (void)dso;
    ThreadAtExit *entry = malloc(sizeof *entry);
    if (!entry)
        return -1;
    entry->destructor = destructor;
    entry->object = object;
    entry->next = pthread_getspecific(thread_atexit_key);
    return pthread_setspecific(thread_atexit_key, entry) ? (free(entry), -1) : 0;
}

/* ---- processes ------------------------------------------------------------------------- */

/* Resolve supplementary membership from the group database before changing credentials.
 * Failure to read the database must not silently discard existing groups. */
int initgroups(const char *user, gid_t base)
{
    FILE *file = fopen("/etc/group", "r");
    if (!file)
        return -1;
    long limit = sysconf(_SC_NGROUPS_MAX);
    if (limit < 1 || (size_t)limit > SIZE_MAX / sizeof(gid_t)) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    gid_t *groups = malloc((size_t)limit * sizeof(*groups));
    if (!groups) {
        fclose(file);
        errno = ENOMEM;
        return -1;
    }
    int count = 1, error = 0;
    groups[0] = base;
    char line[4096];
    errno = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            error = EOVERFLOW;
            break;
        }
        char *cursor = line;
        char *name = strsep(&cursor, ":");
        char *password = strsep(&cursor, ":");
        char *number = strsep(&cursor, ":");
        if (!name || !*name || *name == '#' || !password || !number || !cursor)
            continue;
        char *end;
        errno = 0;
        unsigned long value = strtoul(number, &end, 10);
        if (errno || end == number || *end || (gid_t)value != value)
            continue;
        cursor[strcspn(cursor, "\r\n")] = '\0';
        char *member;
        while ((member = strsep(&cursor, ","))) {
            if (strcmp(member, user))
                continue;
            int i;
            for (i = 0; i < count && groups[i] != (gid_t)value; ++i) {}
            if (i == count) {
                if (count == limit) {
                    error = EINVAL;
                    break;
                }
                groups[count++] = (gid_t)value;
            }
            break;
        }
        if (error)
            break;
        errno = 0;
    }
    if (!error && !feof(file))
        error = errno ? errno : EIO;
    fclose(file);
    int result = -1;
    if (!error) {
        result = setgroups(count, groups);
        error = errno;
    }
    free(groups);
    errno = error;
    return result;
}

/* The PS5 kernel reports a normal exit with bit 0x80 (WCOREFLAG) set: a child that called
 * exit(0) comes back as 0x80, which callers that compare the raw status with 0 (Rust's
 * ExitStatus::success, probe_rust) take as failure. The core flag only means something for a
 * signal, so it is cleared from exit statuses. */
pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t ps5compat_checked_wait4(pid_t pid, int *status, int options, struct rusage *usage);

static void normalize_wait_status(pid_t result, int *status)
{
    if (result > 0 && status && (*status & 0x7f) == 0)
        *status &= ~0x80;
}

pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    pid_t result = __real_waitpid(pid, status, options);
    normalize_wait_status(result, status);
    return result;
}

pid_t __wrap_wait4(pid_t pid, int *status, int options, struct rusage *usage)
{
    pid_t result = ps5compat_checked_wait4(pid, status, options, usage);
    normalize_wait_status(result, status);
    return result;
}

pid_t __wrap_wait(int *status)
{
    return __wrap_waitpid(-1, status, 0);
}

/* A payload cannot duplicate itself: processes are created only by posix_spawn (spawn.c). */
pid_t __wrap_fork(void)
{
    errno = ENOSYS;
    return -1;
}

pid_t __wrap_vfork(void)
{
    errno = ENOSYS;
    return -1;
}

/* ---- terminal ---------------------------------------------------------------------------- */

/* Socket-backed terminals have no device pathname or PTY pair. */
int ttyname_r(int fd, char *name, size_t size)
{
    (void)name;
    (void)size;
    if (fcntl(fd, F_GETFD) < 0)
        return errno;
    return ENOTTY;
}

int openpty(int *master, int *slave, char *name,
            const struct termios *term, const struct winsize *size)
{
    (void)master;
    (void)slave;
    (void)name;
    (void)term;
    (void)size;
    errno = ENOSYS;
    return -1;
}

/* PS5Term gives a program a socket for its standard streams and sets PS5TERM_TTY=1. */
static int ps5term_stream(int fd)
{
    if (fd < 0 || fd > 2)
        return 0;
    const char *tty = getenv("PS5TERM_TTY");
    struct stat st;
    return tty && !strcmp(tty, "1") && !fstat(fd, &st) && S_ISSOCK(st.st_mode);
}

int __wrap_isatty(int fd)
{
    if (__real_isatty(fd))
        return 1;
    int saved = errno;
    if (ps5term_stream(fd))
        return 1;
    errno = saved;
    return 0;
}

static unsigned short env_dimension(const char *name, unsigned short fallback)
{
    const char *text = getenv(name);
    long value = text ? strtol(text, NULL, 10) : 0;
    return value > 0 && value < 10000 ? (unsigned short)value : fallback;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list list;
    va_start(list, request);
    void *argument = va_arg(list, void *);
    va_end(list);
    int result = __real_ioctl(fd, request, argument);
    if (result == 0 || !ps5term_stream(fd))
        return result;
    if (request == TIOCGWINSZ && argument)
    {
        struct winsize *size = argument;
        memset(size, 0, sizeof *size);
        size->ws_col = env_dimension("COLUMNS", 80);
        size->ws_row = env_dimension("LINES", 24);
        return 0;
    }
    return result;
}

/* ---- native addon ------------------------------------------------------------------------ */

/* Provided by the pi-natives staticlib when it is linked into bun.elf. */
extern void *napi_register_module_v1(void *env, void *exports) __attribute__((weak));
extern int node_api_module_get_api_version_v1(void) __attribute__((weak));

static char addon_handle;
static _Thread_local const char *pending_dlerror;

static int ends_with(const char *text, const char *suffix)
{
    size_t length = strlen(text), suffix_length = strlen(suffix);
    return length >= suffix_length && !strcmp(text + length - suffix_length, suffix);
}

void *__wrap_dlopen(const char *path, int mode)
{
    if (path && ends_with(path, PS5COMPAT_ADDON_SUFFIX))
    {
        if (napi_register_module_v1)
            return &addon_handle;
        pending_dlerror = "ps5: the pi-natives addon is not linked into this bun.elf";
        return NULL;
    }
    if (path && ends_with(path, ".node"))
    {
        pending_dlerror = "ps5: dynamic native modules are not supported";
        return NULL;
    }
    return __real_dlopen(path, mode);
}

void *__wrap_dlsym(void *handle, const char *name)
{
    if (handle != &addon_handle)
        return __real_dlsym(handle, name);
    if (!strcmp(name, "napi_register_module_v1"))
        return (void *)napi_register_module_v1;
    if (!strcmp(name, "node_api_module_get_api_version_v1") && node_api_module_get_api_version_v1)
        return (void *)node_api_module_get_api_version_v1;
    pending_dlerror = "ps5: symbol not found in the linked pi-natives addon";
    return NULL;
}

int __wrap_dlclose(void *handle)
{
    return handle == &addon_handle ? 0 : __real_dlclose(handle);
}

char *__wrap_dlerror(void)
{
    const char *error = pending_dlerror;
    if (error)
    {
        pending_dlerror = NULL;
        return (char *)error;
    }
    return __real_dlerror();
}
