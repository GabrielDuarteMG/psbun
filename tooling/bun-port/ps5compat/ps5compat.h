/* PS5Term - ps5compat: what Bun and the omp addon need from FreeBSD that the PS5 payload SDK
 * v0.45 (Sony libc + libkernel, FreeBSD 11 ABI) does not provide. GPL-3.0-or-later.
 *
 * Link: -Wl,--whole-archive -lps5compat -Wl,--no-whole-archive plus every flag listed in
 * ps5compat.ldflags (the --wrap list for functions the SDK already defines). Nothing here
 * issues a raw syscall number; everything goes through the SDK's libc/libkernel, except the
 * process creation inherited from shsrv's elfldr_spawn (third_party/shsrv/elfldr.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* eventfd(2) emulated with a pipe and a counter; the descriptor works with read, write,
 * close, poll and kqueue EVFILT_READ. dup() of an eventfd descriptor is not supported. */
#define EFD_SEMAPHORE 0x00000001
#define EFD_NONBLOCK 0x00000004   /* O_NONBLOCK */
#define EFD_CLOEXEC 0x00100000    /* O_CLOEXEC */
typedef uint64_t eventfd_t;
int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

/* elf_aux_info(3): AT_PAGESZ (hw.pagesize) and AT_NCPUS (hw.ncpu); anything else ENOENT. */
int elf_aux_info(int aux, void *buf, int buflen);

/* getrandom(2)/getentropy(3) from /dev/urandom. */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int getentropy(void *buf, size_t buflen);

/* copy_file_range(2) is absent from the PS5 kernel: implemented with read/write (ps5compat.c). */
ssize_t copy_file_range(int infd, off_t *inoffp, int outfd, off_t *outoffp, size_t len,
                        unsigned int flags);

/* FreeBSD 14 posix_spawn additions. */
typedef struct __posix_spawn_file_actions *ps5compat_file_actions_t;
int posix_spawn_file_actions_addchdir_np(ps5compat_file_actions_t *actions, const char *path);
int posix_spawn_file_actions_addfchdir_np(ps5compat_file_actions_t *actions, int fd);
int posix_spawn_file_actions_addclosefrom_np(ps5compat_file_actions_t *actions, int from);

/* The bun.elf used for "#!/usr/bin/env bun" scripts and for spawning "bun":
 * $PS5COMPAT_BUN, else /data/ps5term/bun/bin/bun.elf. */
const char *ps5compat_bun_path(void);

/* The native addon linked into bun.elf, exposed through dlopen() of any path ending in
 * PS5COMPAT_ADDON_SUFFIX. */
#define PS5COMPAT_ADDON_SUFFIX "pi_natives.freebsd-x64-baseline.node"

#ifdef __cplusplus
}
#endif
