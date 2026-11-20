/* PS5Term - ps5compat posix_spawn on top of shsrv's elfldr_spawn. GPL-3.0-or-later.
 *
 * A PS5 payload cannot fork or execve its own ELF files. The only proven way to start one is
 * elfldr_spawn (rfork + execve of SceSpZeroConf + ptrace, third_party/shsrv/elfldr.c), so
 * posix_spawn is implemented with it. The new process gets the target descriptors 0, 1 and 2
 * computed from the file actions; descriptors above 2 are never inherited (dup2/open onto
 * them fail with ENOTSUP, close of them is naturally satisfied). Executables:
 *   - an ELF file is loaded as is;
 *   - a script whose "#!" interpreter is bun (directly, or through env) runs in bun.elf;
 *   - anything else fails with ENOEXEC.
 * POSIX_SPAWN_SETPGROUP/SETSID cannot be applied to a process the loader already started; the
 * child stays in the caller's session and process group. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elfldr.h"
#include "ps5compat.h"

extern char **environ;
int __real_close(int fd);

enum ActionKind
{
    ACTION_OPEN,
    ACTION_DUP2,
    ACTION_CLOSE,
    ACTION_CHDIR,
    ACTION_FCHDIR,
    ACTION_CLOSEFROM,
};

typedef struct
{
    enum ActionKind kind;
    int fd, newfd, oflag;
    mode_t mode;
    char *path;
} Action;

struct __posix_spawn_file_actions
{
    Action *items;
    size_t count, capacity;
};

struct __posix_spawnattr
{
    short flags;
    pid_t pgroup;
    sigset_t mask, defaults;
    int policy;
    struct sched_param param;
};

/* ---- file actions ------------------------------------------------------------------------ */

int __wrap_posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{
    *actions = calloc(1, sizeof **actions);
    return *actions ? 0 : ENOMEM;
}

int __wrap_posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)
{
    if (!actions || !*actions)
        return EINVAL;
    for (size_t i = 0; i < (*actions)->count; i++)
        free((*actions)->items[i].path);
    free((*actions)->items);
    free(*actions);
    *actions = NULL;
    return 0;
}

static int add_action(posix_spawn_file_actions_t *actions, Action action)
{
    if (!actions || !*actions)
        return EINVAL;
    struct __posix_spawn_file_actions *list = *actions;
    if (list->count == list->capacity)
    {
        size_t capacity = list->capacity ? list->capacity * 2 : 8;
        Action *items = realloc(list->items, capacity * sizeof *items);
        if (!items)
            return ENOMEM;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = action;
    return 0;
}

int __wrap_posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd,
                                            const char *path, int oflag, mode_t mode)
{
    if (fd < 0)
        return EBADF;
    char *copy = strdup(path);
    if (!copy)
        return ENOMEM;
    int error = add_action(actions, (Action){ACTION_OPEN, fd, -1, oflag, mode, copy});
    if (error)
        free(copy);
    return error;
}

int __wrap_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd,
                                            int newfd)
{
    if (fd < 0 || newfd < 0)
        return EBADF;
    return add_action(actions, (Action){ACTION_DUP2, fd, newfd, 0, 0, NULL});
}

int __wrap_posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd)
{
    if (fd < 0)
        return EBADF;
    return add_action(actions, (Action){ACTION_CLOSE, fd, -1, 0, 0, NULL});
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *actions, const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return ENOMEM;
    int error = add_action(actions, (Action){ACTION_CHDIR, -1, -1, 0, 0, copy});
    if (error)
        free(copy);
    return error;
}

int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *actions, int fd)
{
    if (fd < 0)
        return EBADF;
    return add_action(actions, (Action){ACTION_FCHDIR, fd, -1, 0, 0, NULL});
}

int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t *actions, int from)
{
    if (from < 0)
        return EBADF;
    return add_action(actions, (Action){ACTION_CLOSEFROM, from, -1, 0, 0, NULL});
}

/* ---- attributes -------------------------------------------------------------------------- */

int __wrap_posix_spawnattr_init(posix_spawnattr_t *attr)
{
    *attr = calloc(1, sizeof **attr);
    return *attr ? 0 : ENOMEM;
}

int __wrap_posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    if (!attr || !*attr)
        return EINVAL;
    free(*attr);
    *attr = NULL;
    return 0;
}

int __wrap_posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    (*attr)->flags = flags;
    return 0;
}

int __wrap_posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{
    *flags = (*attr)->flags;
    return 0;
}

int __wrap_posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{
    (*attr)->pgroup = pgroup;
    return 0;
}

int __wrap_posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)
{
    *pgroup = (*attr)->pgroup;
    return 0;
}

int __wrap_posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *mask)
{
    (*attr)->mask = *mask;
    return 0;
}

int __wrap_posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *mask)
{
    *mask = (*attr)->mask;
    return 0;
}

int __wrap_posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *defaults)
{
    (*attr)->defaults = *defaults;
    return 0;
}

int __wrap_posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *defaults)
{
    *defaults = (*attr)->defaults;
    return 0;
}

int __wrap_posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy)
{
    (*attr)->policy = policy;
    return 0;
}

int __wrap_posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *policy)
{
    *policy = (*attr)->policy;
    return 0;
}

int __wrap_posix_spawnattr_setschedparam(posix_spawnattr_t *attr, const struct sched_param *param)
{
    (*attr)->param = *param;
    return 0;
}

int __wrap_posix_spawnattr_getschedparam(const posix_spawnattr_t *attr, struct sched_param *param)
{
    *param = (*attr)->param;
    return 0;
}

/* ---- executable resolution --------------------------------------------------------------- */

const char *ps5compat_bun_path(void)
{
    const char *path = getenv("PS5COMPAT_BUN");
    return path && *path ? path : "/data/ps5term/bun/bin/bun.elf";
}

typedef struct
{
    uint8_t *bytes;
    size_t size;
} Image;

static int map_file(const char *path, Image *image)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    struct stat st;
    if (fstat(fd, &st))
    {
        int error = errno;
        __real_close(fd);
        return error;
    }
    if (!S_ISREG(st.st_mode))
    {
        __real_close(fd);
        return S_ISDIR(st.st_mode) ? EISDIR : EACCES;
    }
    if (st.st_size < 4)
    {
        __real_close(fd);
        return ENOEXEC;
    }
    void *bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    int error = bytes == MAP_FAILED ? errno : 0;
    __real_close(fd);
    if (error)
        return error;
    image->bytes = bytes;
    image->size = (size_t)st.st_size;
    return 0;
}

static void unmap_file(Image *image)
{
    if (image->bytes)
        munmap(image->bytes, image->size);
    image->bytes = NULL;
}

static int is_bun_name(const char *word, size_t length)
{
    const char *base = word;
    for (size_t i = 0; i < length; i++)
        if (word[i] == '/')
            base = word + i + 1;
    size_t base_length = length - (size_t)(base - word);
    return (base_length == 3 && !memcmp(base, "bun", 3)) ||
           (base_length == 7 && !memcmp(base, "bun.elf", 7));
}

/* Script interpreted by bun? Sets *extra to the optional interpreter argument (owned). */
static int bun_script(const Image *image, char **extra)
{
    *extra = NULL;
    if (image->size < 2 || memcmp(image->bytes, "#!", 2))
        return 0;
    size_t end = 2;
    while (end < image->size && end < 512 && image->bytes[end] != '\n')
        end++;
    const char *line = (const char *)image->bytes + 2;
    size_t length = end - 2;
    const char *words[4];
    size_t lengths[4];
    int count = 0;
    for (size_t i = 0; i < length && count < 4;)
    {
        while (i < length && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            i++;
        if (i == length)
            break;
        words[count] = line + i;
        while (i < length && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            i++;
        lengths[count] = (size_t)(line + i - words[count]);
        count++;
    }
    if (count == 0)
        return 0;
    int next = 1;
    if (lengths[0] >= 3 && !memcmp(words[0] + lengths[0] - 3, "env", 3) &&
        (lengths[0] == 3 || words[0][lengths[0] - 4] == '/'))
    {
        if (next < count && lengths[next] == 2 && !memcmp(words[next], "-S", 2))
            next++;
        if (next >= count || !is_bun_name(words[next], lengths[next]))
            return 0;
        next++;
    }
    else if (!is_bun_name(words[0], lengths[0]))
        return 0;
    if (next < count)
    {
        *extra = strndup(words[next], lengths[next]);
        if (!*extra)
            return -1;
    }
    return 1;
}

/* PATH search for posix_spawnp; "name" also matches "name.elf" (PS5 programs). */
static int search_path(const char *name, char *out, size_t size)
{
    if (strchr(name, '/'))
    {
        if (strlen(name) >= size)
            return ENAMETOOLONG;
        strcpy(out, name);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/data/ps5term/bun/bin";
    int error = ENOENT;
    for (const char *dir = path;; dir = strchr(dir, ':') + 1)
    {
        size_t length = strcspn(dir, ":");
        static const char *const suffixes[] = {"", ".elf"};
        for (int s = 0; s < 2; s++)
        {
            int wrote = snprintf(out, size, "%.*s%s%s%s", (int)length, length ? dir : ".",
                                 length && dir[length - 1] == '/' ? "" : "/", name, suffixes[s]);
            if (wrote < 0 || (size_t)wrote >= size)
            {
                error = ENAMETOOLONG;
                continue;
            }
            struct stat st;
            if (!stat(out, &st) && S_ISREG(st.st_mode))
                return 0;
        }
        if (!strchr(dir, ':'))
            return error;
    }
}

/* ---- spawn ------------------------------------------------------------------------------- */

static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;

static int map_parent_fd(const int stdio[3], int fd)
{
    return fd >= 0 && fd <= 2 ? stdio[fd] : fd;
}

static int do_spawn(pid_t *out_pid, const char *file, const posix_spawn_file_actions_t *actions,
                    char *const argv[], char *const envp[])
{
    Image image = {0};
    char *extra = NULL;
    char **child_argv = NULL;
    int opened[16], opened_count = 0;
    int error = map_file(file, &image);
    if (error)
        return error;

    int argc = 0;
    while (argv && argv[argc])
        argc++;
    if (!memcmp(image.bytes, "\177ELF", 4))
    {
        child_argv = calloc((size_t)argc + 2, sizeof *child_argv);
        if (!child_argv)
        {
            error = ENOMEM;
            goto done;
        }
        for (int i = 0; i < argc; i++)
            child_argv[i] = argv[i];
        if (!argc)
            child_argv[0] = (char *)file;
    }
    else
    {
        int script = bun_script(&image, &extra);
        unmap_file(&image);
        if (script <= 0)
        {
            error = script < 0 ? ENOMEM : ENOEXEC;
            goto done;
        }
        const char *bun = ps5compat_bun_path();
        if ((error = map_file(bun, &image)))
            goto done;
        if (memcmp(image.bytes, "\177ELF", 4))
        {
            error = ENOEXEC;
            goto done;
        }
        child_argv = calloc((size_t)argc + 4, sizeof *child_argv);
        if (!child_argv)
        {
            error = ENOMEM;
            goto done;
        }
        int n = 0;
        child_argv[n++] = (char *)bun;
        if (extra)
            child_argv[n++] = extra;
        child_argv[n++] = (char *)file;
        for (int i = 1; i < argc; i++)
            child_argv[n++] = argv[i];
    }

    /* Descriptors 0-2 of the child, as parent descriptors (-1: closed). */
    int stdio[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    char cwd_buffer[PATH_MAX];
    const char *cwd = getcwd(cwd_buffer, sizeof cwd_buffer);
    char chdir_buffer[PATH_MAX];
    for (size_t i = 0; actions && *actions && i < (*actions)->count; i++)
    {
        const Action *action = &(*actions)->items[i];
        switch (action->kind)
        {
        case ACTION_DUP2:
            if (action->newfd > 2)
            {
                error = ENOTSUP;
                goto done;
            }
            stdio[action->newfd] = map_parent_fd(stdio, action->fd);
            break;
        case ACTION_OPEN:
        {
            if (action->fd > 2)
            {
                error = ENOTSUP;
                goto done;
            }
            char resolved[PATH_MAX];
            const char *path = action->path;
            if (path[0] != '/' && cwd)
            {
                snprintf(resolved, sizeof resolved, "%s/%s", cwd, path);
                path = resolved;
            }
            int fd = open(path, action->oflag | O_CLOEXEC, action->mode);
            if (fd < 0 || opened_count == (int)(sizeof opened / sizeof *opened))
            {
                error = fd < 0 ? errno : EMFILE;
                if (fd >= 0)
                    __real_close(fd);
                goto done;
            }
            opened[opened_count++] = fd;
            stdio[action->fd] = fd;
            break;
        }
        case ACTION_CLOSE:
            if (action->fd <= 2)
                stdio[action->fd] = -1;
            break;
        case ACTION_CLOSEFROM:
            for (int fd = action->fd; fd <= 2; fd++)
                stdio[fd] = -1;
            break;
        case ACTION_CHDIR:
            if (action->path[0] == '/' || !cwd)
                snprintf(chdir_buffer, sizeof chdir_buffer, "%s", action->path);
            else
                snprintf(chdir_buffer, sizeof chdir_buffer, "%s/%s", cwd, action->path);
            cwd = chdir_buffer;
            break;
        case ACTION_FCHDIR:
            error = ENOTSUP; /* No portable fd-to-path lookup on the PS5 kernel. */
            goto done;
        }
    }

    pthread_mutex_lock(&spawn_lock);
    pid_t pid = elfldr_spawn_env(stdio[0], stdio[1], stdio[2], image.bytes, child_argv,
                                 (char **)(envp ? envp : environ), cwd);
    int spawn_errno = errno;
    pthread_mutex_unlock(&spawn_lock);
    if (pid < 0)
        error = spawn_errno && spawn_errno != EINTR ? spawn_errno : EAGAIN;
    else if (out_pid)
        *out_pid = pid;

done:
    for (int i = 0; i < opened_count; i++)
        __real_close(opened[i]);
    unmap_file(&image);
    free(child_argv);
    free(extra);
    return error;
}

int __wrap_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    (void)attr;
    return do_spawn(pid, path, actions, argv, envp);
}

int __wrap_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                        const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    (void)attr;
    char path[PATH_MAX];
    int error = search_path(file, path, sizeof path);
    return error ? error : do_spawn(pid, path, actions, argv, envp);
}
