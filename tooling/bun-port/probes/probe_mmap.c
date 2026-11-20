/* PS5Term - Bun port probe: the largest anonymous mmap (PROT_NONE and RW) this process
 * gets, in 16 MiB steps up to 1 GiB, then how much RW memory it can map and touch in
 * 16 MiB chunks (up to 512 MiB) and the mmap errno at the limit. Prints to stdout and the
 * klog. GPL-3.0-or-later. */
#include <sys/mman.h>

#include "probe.h"

static size_t largest(int prot)
{
    size_t best = 0;
    for (size_t size = 16u << 20; size <= (1024u << 20); size += 16u << 20)
    {
        void *p = mmap(NULL, size, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
        {
            printf("prot=%d fails at %zu MiB errno=%d\n", prot, size >> 20, errno);
            break;
        }
        munmap(p, size);
        best = size;
    }
    return best;
}

int main(void)
{
    PROBE_START("probe_mmap");
    size_t none = largest(PROT_NONE), rw = largest(PROT_READ | PROT_WRITE);
    printf("largest PROT_NONE=%zu MiB RW=%zu MiB\n", none >> 20, rw >> 20);
    enum { CHUNK = 16 << 20, MAX = 32 };
    char *chunks[MAX];
    int count = 0, error = 0;
    for (; count < MAX; count++)
    {
        char *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
        {
            error = errno;
            break;
        }
        memset(p, 1, CHUNK);
        chunks[count] = p;
    }
    for (int i = 0; i < count; i++)
        munmap(chunks[i], CHUNK);
    printf("touched=%d MiB errno=%d\n", count * 16, error);
    fflush(stdout);
    PROBE_LOG("probe_mmap", "largest PROT_NONE=%zu MiB RW=%zu MiB touched=%d MiB errno=%d",
              none >> 20, rw >> 20, count * 16, error);
    return PROBE_PASS("probe_mmap");
}
