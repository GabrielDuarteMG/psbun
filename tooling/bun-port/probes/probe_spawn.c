/* PS5Term - probe_spawn: ps5compat posix_spawn starts probe_child.elf with its stdout on a
 * pipe; the parent reads "child-ok", sees NOTE_EXIT in kqueue and reaps status 0.
 * Argument 1: path of probe_child.elf on the console. GPL-3.0-or-later. */
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/wait.h>

#include "probe.h"
#include "ps5compat.h"

#define NAME "probe_spawn"

int main(void)
{
    PROBE_START(NAME);
    char storage[1024], *args[4];
    if (probe_args(storage, sizeof storage, args, 4) < 1)
        return PROBE_FAIL(NAME, "missing child path argument", EINVAL);
    int fds[2];
    if (pipe(fds))
        return PROBE_FAIL(NAME, "pipe", errno);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (error)
        return PROBE_FAIL(NAME, "file actions", error);
    char *child_argv[] = {args[0], "hello", NULL};
    char *child_envp[] = {"PROBE_CHILD=1", NULL};
    pid_t pid = -1;
    error = posix_spawn(&pid, args[0], &actions, NULL, child_argv, child_envp);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (error)
        return PROBE_FAIL(NAME, "posix_spawn", error);
    PROBE_LOG(NAME, "child pid %d", (int)pid);

    int kq = kqueue();
    struct kevent change, event;
    EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    int watching = kq >= 0 && kevent(kq, &change, 1, NULL, 0, NULL) == 0;
    if (!watching)
        PROBE_LOG(NAME, "EVFILT_PROC registration failed: %s", strerror(errno));

    char output[256];
    size_t used = 0;
    for (;;)
    {
        ssize_t got = read(fds[0], output + used, sizeof output - 1 - used);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        used += (size_t)got;
        if (used == sizeof output - 1)
            break;
    }
    output[used] = '\0';
    close(fds[0]);
    PROBE_LOG(NAME, "child wrote: %s", output);

    int exited_event = 0;
    if (watching)
    {
        struct timespec timeout = {10, 0};
        exited_event = kevent(kq, NULL, 0, &event, 1, &timeout) == 1 &&
                       event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT);
    }
    int status = -1;
    pid_t done = waitpid(pid, &status, 0);
    if (done != pid)
        return PROBE_FAIL(NAME, "waitpid", errno);
    if (strcmp(output, "child-ok hello PROBE_CHILD=1\n"))
        return PROBE_FAIL(NAME, "unexpected child output", EINVAL);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        PROBE_LOG(NAME, "child status 0x%x", status);
        return PROBE_FAIL(NAME, "child exit status", EINVAL);
    }
    if (!exited_event)
        return PROBE_FAIL(NAME, "kqueue NOTE_EXIT", ETIMEDOUT);
    PROBE_LOG(NAME, "stdout pipe, argv, envp, NOTE_EXIT and waitpid status 0 ok");
    return PROBE_PASS(NAME);
}
