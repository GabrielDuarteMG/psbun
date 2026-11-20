/* PS5Term - ps5compat: FreeBSD libexecinfo (backtrace(3)) on top of the SDK's libunwind, for
 * WTF's and Bun's crash reports. Symbol names are not available in a payload, so
 * backtrace_symbols() prints addresses. GPL-3.0-or-later. */
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <link.h>
#include <unwind.h>

typedef struct
{
    void **frames;
    size_t size, used;
} BacktraceState;

static _Unwind_Reason_Code collect_frame(struct _Unwind_Context *context, void *argument)
{
    BacktraceState *state = argument;
    uintptr_t ip = _Unwind_GetIP(context);
    if (!ip)
        return _URC_END_OF_STACK;
    if (state->used == state->size)
        return _URC_END_OF_STACK;
    state->frames[state->used++] = (void *)ip;
    return _URC_NO_REASON;
}

size_t backtrace(void **frames, size_t size)
{
    BacktraceState state = {frames, size, 0};
    _Unwind_Backtrace(collect_frame, &state);
    return state.used;
}

char **backtrace_symbols(void *const *frames, size_t count)
{
    /* One block: the pointer array followed by "0x%016lx" strings, freed with one free(). */
    const size_t width = 2 + 16 + 1;
    char **result = malloc(count * (sizeof(char *) + width));
    if (!result)
        return NULL;
    char *text = (char *)(result + count);
    for (size_t i = 0; i < count; i++, text += width)
    {
        snprintf(text, width, "0x%016lx", (unsigned long)(uintptr_t)frames[i]);
        result[i] = text;
    }
    return result;
}

int backtrace_symbols_fd(void *const *frames, size_t count, int fd)
{
    for (size_t i = 0; i < count; i++)
    {
        char line[32];
        int size = snprintf(line, sizeof line, "0x%016lx\n", (unsigned long)(uintptr_t)frames[i]);
        if (write(fd, line, (size_t)size) != size)
            return -1;
    }
    return 0;
}

/* dl_iterate_phdr(3) for the payload itself. The loader does not map the ELF headers, so the
 * program headers are rebuilt from the SDK linker script's boundary symbols: one PT_LOAD for the
 * whole image and PT_GNU_EH_FRAME for the unwinder. There are no other modules to report. */
extern char __image_start[] __attribute__((weak, visibility("hidden")));
extern char __image_end[] __attribute__((weak, visibility("hidden")));
extern char __eh_frame_hdr_start[] __attribute__((weak, visibility("hidden")));
extern char __eh_frame_hdr_end[] __attribute__((weak, visibility("hidden")));

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data)
{
    if (!__image_start || !__image_end)
        return 0;
    uintptr_t base = (uintptr_t)__image_start;
    Elf_Phdr headers[2];
    memset(headers, 0, sizeof headers);
    headers[0].p_type = PT_LOAD;
    headers[0].p_flags = PF_R | PF_W | PF_X;
    headers[0].p_memsz = headers[0].p_filesz = (uintptr_t)__image_end - base;
    headers[0].p_align = (Elf_Size)sysconf(_SC_PAGESIZE);
    int count = 1;
    if (__eh_frame_hdr_start && __eh_frame_hdr_end > __eh_frame_hdr_start)
    {
        headers[1].p_type = PT_GNU_EH_FRAME;
        headers[1].p_flags = PF_R;
        headers[1].p_vaddr = headers[1].p_paddr = (uintptr_t)__eh_frame_hdr_start - base;
        headers[1].p_memsz = headers[1].p_filesz = (uintptr_t)(__eh_frame_hdr_end - __eh_frame_hdr_start);
        headers[1].p_align = 4;
        count = 2;
    }
    struct dl_phdr_info info;
    memset(&info, 0, sizeof info);
    info.dlpi_addr = base;
    info.dlpi_name = "";
    info.dlpi_phdr = headers;
    info.dlpi_phnum = (Elf_Half)count;
    info.dlpi_adds = 1;
    return callback(&info, sizeof info, data);
}
