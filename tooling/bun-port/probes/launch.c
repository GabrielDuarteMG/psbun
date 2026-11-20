/* PS5Term - launch: development launcher for Bun-port gates. GPL-3.0-or-later.
 *
 * Reads /data/ps5term-dev/args: line 1 the TCP port to listen on, line 2 the program, the rest
 * its arguments. Accepts one connection from the host (30 s at most), then starts the program
 * through ps5compat's posix_spawn (elfldr_spawn, as ps5term-run does) with that socket as
 * stdin, stdout and stderr, waits for it and reports "PASS launch <status>" / "FAIL launch
 * <errno>" in klog. The environment mirrors a PS5Term tab (ps5term-run.c). */
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "probe.h"
#include "ps5compat.h"

#define NAME "launch"

int main(void)
{
    PROBE_START(NAME);
    static char storage[16384];
    char *args[64];
    int count = probe_args(storage, sizeof storage, args, 63);
    if (count < 2)
        return PROBE_FAIL(NAME, "args: need a port and a program", EINVAL);
    args[count] = NULL;
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)atoi(args[0]));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    int server = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof address) || listen(server, 1))
        return PROBE_FAIL(NAME, "listen", errno);
    PROBE_LOG(NAME, "listening on %s", args[0]);
    struct pollfd wait_client = {server, POLLIN, 0};
    if (poll(&wait_client, 1, 30000) != 1)
        return PROBE_FAIL(NAME, "no host connection", ETIMEDOUT);
    int sock = accept(server, NULL, NULL);
    close(server);
    if (sock < 0)
        return PROBE_FAIL(NAME, "accept", errno);

    setenv("TERM", "xterm-256color", 1);
    setenv("HOME", "/data", 1);
    setenv("PWD", "/data/ps5term-dev", 1);
    setenv("BUN_INSTALL", "/data/ps5term/bun", 1);
    setenv("PATH", "/data/ps5term/bun/bin", 1);
    chdir("/data/ps5term-dev");

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sock, 0);
    posix_spawn_file_actions_adddup2(&actions, sock, 1);
    posix_spawn_file_actions_adddup2(&actions, sock, 2);
    pid_t pid = -1;
    extern char **environ;
    int error = posix_spawn(&pid, args[1], &actions, NULL, args + 1, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sock);
    if (error)
        return PROBE_FAIL(NAME, "posix_spawn", error);
    PROBE_LOG(NAME, "child PID %d", (int)pid);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return PROBE_FAIL(NAME, "waitpid", errno);
    if (WIFSIGNALED(status))
    {
        PROBE_LOG(NAME, "child killed by signal %d", WTERMSIG(status));
        klog_printf("PASS %s signal=%d\n", NAME, WTERMSIG(status));
        return 0;
    }
    klog_printf("PASS %s exit=%d\n", NAME, WEXITSTATUS(status));
    return 0;
}
