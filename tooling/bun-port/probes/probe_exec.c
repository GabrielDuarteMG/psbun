/* PS5Term - probe_exec: anonymous RW memory turned RX with mprotect and executed.
 * GPL-3.0-or-later. */
#include <sys/mman.h>

#include "probe.h"

#define NAME "probe_exec"

int main(void)
{
    PROBE_START(NAME);
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    PROBE_LOG(NAME, "page size %zu", size);
    unsigned char *page = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
        return PROBE_FAIL(NAME, "mmap RW", errno);
    static const unsigned char code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3}; /* mov eax,42; ret */
    memcpy(page, code, sizeof code);
    if (mprotect(page, size, PROT_READ | PROT_EXEC))
        return PROBE_FAIL(NAME, "mprotect RX", errno);
    int (*function)(void) = (int (*)(void))(void *)page;
    int result = function();
    if (result != 42)
    {
        PROBE_LOG(NAME, "generated code returned %d", result);
        return PROBE_FAIL(NAME, "generated code", EINVAL);
    }
    /* Back to RW and again RX, as an interpreter's code cache would. */
    if (mprotect(page, size, PROT_READ | PROT_WRITE))
        return PROBE_FAIL(NAME, "mprotect RW again", errno);
    page[1] = 0x07;
    if (mprotect(page, size, PROT_READ | PROT_EXEC))
        return PROBE_FAIL(NAME, "mprotect RX again", errno);
    if ((result = function()) != 7)
        return PROBE_FAIL(NAME, "rewritten code", EINVAL);
    munmap(page, size);
    PROBE_LOG(NAME, "RW->RX code returned 42, rewritten code returned 7");
    return PROBE_PASS(NAME);
}
