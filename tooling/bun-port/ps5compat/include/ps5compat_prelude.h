/* PS5Term - ps5compat prelude, force-included (-include) into every C/C++ file compiled for the
 * Bun port. Declares FreeBSD 12-14 libc functions that the PS5 payload SDK's FreeBSD 11 headers
 * lack; ps5compat (or the SDK's libkernel) defines them. Declarations only, no includes, so
 * include order and feature macros of the including file are untouched. GPL-3.0-or-later. */
#ifndef PS5COMPAT_PRELUDE_H
#define PS5COMPAT_PRELUDE_H
#ifndef __ASSEMBLER__

#ifdef __cplusplus
extern "C" {
#endif

struct pthread;
struct sockaddr;
/* In libkernel without a declaration in the SDK headers. */
int pthread_getname_np(struct pthread *thread, char *name, __SIZE_TYPE__ size);
/* ps5compat. */
int pthread_setname_np(struct pthread *thread, const char *name);
char *strchrnul(const char *s, int c);
void *memmem(const void *big, __SIZE_TYPE__ big_len, const void *little, __SIZE_TYPE__ little_len);
int timingsafe_bcmp(const void *a, const void *b, __SIZE_TYPE__ size);
void explicit_bzero(void *buf, __SIZE_TYPE__ size);
int dup3(int oldfd, int newfd, int flags);
int close_range(unsigned int low, unsigned int high, int flags);
int accept4(int s, struct sockaddr *__restrict addr, unsigned int *__restrict addrlen, int flags);
long getrandom(void *buf, __SIZE_TYPE__ buflen, unsigned int flags);
int getentropy(void *buf, __SIZE_TYPE__ buflen);

#ifdef __cplusplus
}
#endif

#endif
#endif
