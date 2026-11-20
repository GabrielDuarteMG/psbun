/* PS5Term - ps5compat: emulated thread-local storage (__emutls_get_address) for PS5 payloads.
 * GPL-3.0-or-later.
 *
 * The payload loader does not set up ELF TLS, so C, C++ and Rust thread-locals are compiled as
 * emulated TLS. The SDK's implementation (compiler-rt's) frees a thread's TLS block from the
 * first pthread key destructor round; its key is created before the keys that C++ and Rust
 * runtimes use to run thread_local destructors, and the PS5 runs key destructors in creation
 * order, so those destructors found their lists already gone and never ran (probe_cxx,
 * probe_rust). This version keeps the block alive for the first rounds by setting its key
 * again (the PS5 honours PTHREAD_DESTRUCTOR_ITERATIONS, probe_keys) and frees it last. Same ABI
 * as compiler-rt's emutls.c. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct __emutls_control
{
    size_t size;  /* Size of the object. */
    size_t align; /* Alignment of the object. */
    union
    {
        uintptr_t index; /* Data[index - 1] holds the object; 0 until first use. */
        void *address;
    } object;
    void *value; /* Initial value, or NULL for zero-fill. */
} __emutls_control;

typedef struct
{
    uintptr_t rounds_left; /* Destructor rounds to survive before freeing. */
    uintptr_t size;        /* Number of slots in data. */
    void *data[];
} EmutlsArray;

/* POSIX guarantees at least 4 destructor iterations; survive all but the last one. */
#define EMUTLS_SKIP_ROUNDS 2

static pthread_key_t emutls_key;
static pthread_once_t emutls_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t emutls_count;

static void emutls_destroy(void *pointer)
{
    EmutlsArray *array = pointer;
    if (array->rounds_left)
    {
        array->rounds_left--;
        pthread_setspecific(emutls_key, array); /* Other destructors may still need TLS. */
        return;
    }
    for (uintptr_t i = 0; i < array->size; i++)
        free(array->data[i]);
    free(array);
}

static void emutls_init(void)
{
    if (pthread_key_create(&emutls_key, emutls_destroy))
        abort();
}

static uintptr_t emutls_index(__emutls_control *control)
{
    uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
    if (index)
        return index;
    pthread_once(&emutls_once, emutls_init);
    pthread_mutex_lock(&emutls_mutex);
    index = control->object.index;
    if (!index)
    {
        index = ++emutls_count;
        __atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&emutls_mutex);
    return index;
}

static void *emutls_allocate(const __emutls_control *control)
{
    size_t align = control->align < sizeof(void *) ? sizeof(void *) : control->align;
    void *object = NULL;
    if (posix_memalign(&object, align, control->size ? control->size : 1))
        abort();
    if (control->value)
        memcpy(object, control->value, control->size);
    else
        memset(object, 0, control->size);
    return object;
}

void *__emutls_get_address(__emutls_control *control)
{
    uintptr_t index = emutls_index(control);
    EmutlsArray *array = pthread_getspecific(emutls_key);
    if (!array || array->size < index)
    {
        uintptr_t old_size = array ? array->size : 0;
        uintptr_t size = old_size ? old_size : 16;
        while (size < index)
            size *= 2;
        EmutlsArray *grown = realloc(array, sizeof *grown + size * sizeof(void *));
        if (!grown)
            abort();
        if (!array)
            grown->rounds_left = EMUTLS_SKIP_ROUNDS;
        memset(grown->data + old_size, 0, (size - old_size) * sizeof(void *));
        grown->size = size;
        array = grown;
        if (pthread_setspecific(emutls_key, array))
            abort();
    }
    void **slot = &array->data[index - 1];
    if (!*slot)
        *slot = emutls_allocate(control);
    return *slot;
}
