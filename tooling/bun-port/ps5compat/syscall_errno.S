/* PS5Term - Bun port: error reporting for the SDK's bare system call stubs.
 *
 * The SDK's libc.a (syscalls.o) defines many calls as `mov $N, %rax; mov %rcx, %r10; syscall;
 * ret` with no error path: when the kernel fails a call it sets the carry flag and puts the error
 * number in %rax, and the stub returns that number as the result. openat() of a missing file thus
 * returns 2 (ENOENT) as if it were descriptor 2, and errno is never set.
 *
 * `ret` leaves the flags alone, so the carry flag the kernel set is still visible after the stub
 * returns. Each wrapper below calls the SDK's own stub (no new system call is issued here), and on
 * carry stores the error in errno and returns -1, as every libc does. Each call has at most six
 * arguments, all in registers, so the extra return address on the stack moves nothing.
 * GPL-3.0-or-later. */

        .text

/* ps5compat_checked_NAME: the checked call; C wrappers that post-process results call this. */
.macro checked name
        .globl ps5compat_checked_\name
        .type ps5compat_checked_\name, @function
ps5compat_checked_\name:
        call __real_\name@PLT
        jc 1f
        ret
1:
        mov %eax, %edi
        jmp ps5compat_fail_errno@PLT
        .size ps5compat_checked_\name, . - ps5compat_checked_\name
.endm

/* checked call that also replaces NAME everywhere through -Wl,--wrap=NAME. */
.macro wrapped name
        .globl __wrap_\name
        .type __wrap_\name, @function
__wrap_\name:
        checked \name
.endm

checked wait4 /* __wrap_wait4 (ps5compat.c) normalizes the status after this call. */

wrapped link
wrapped fchdir
wrapped chown
wrapped symlink
wrapped readlink
wrapped fchown
wrapped setgid
wrapped minherit
wrapped lchown
wrapped lchmod
wrapped extattr_get_fd
wrapped statfs
wrapped extattr_list_fd
wrapped fchmodat
wrapped fstatat
wrapped linkat
wrapped mkdirat
checked openat /* __wrap_openat (fdpath.c) records the descriptor's path after this call. */
wrapped renameat
wrapped symlinkat
wrapped unlinkat
wrapped procctl
wrapped ppoll
/* Not wrapped here: faccessat, readlinkat, futimens and utimensat. The PS5 kernel answers the
 * first two with ENOSYS, utimensat with EACCES and futimens with ESRCH (console probe_faccessat),
 * so fdpath.c implements them with access/fstatat/readlink/utimes/futimes and never calls them. */

        .section .note.GNU-stack, "", @progbits
