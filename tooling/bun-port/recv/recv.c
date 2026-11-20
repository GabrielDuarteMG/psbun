/* PS5Term - recv: development-only file receiver for the Bun port. GPL-3.0-or-later.
 *
 * Sent to the console's ELF loader (port 9021) by tools/dev/ps5_bun_console.py. Listens on
 * TCP 9300 for exactly one transfer "<path>\n<size>\n<bytes>", refuses any path outside
 * /data/ps5term-dev/ and /data/ps5term/bun/, writes "<path>.partial", renames it into place
 * and answers "OK <sha256>\n" (or "ERR <reason>\n"), then exits. A 300 s alarm (room for a
 * ~250 MB bun.elf) bounds its lifetime so it never lingers on the console. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/klog.h>

#include "sha256.h"

#define RECV_PORT 9300
#define MAX_FILE (512LL * 1024 * 1024)

static const char *const allowed_roots[] = {"/data/ps5term-dev/", "/data/ps5term/bun/"};

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size)
    {
        ssize_t wrote = write(fd, bytes, size);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return -1;
        bytes += wrote;
        size -= (size_t)wrote;
    }
    return 0;
}

static void reply(int fd, const char *text)
{
    write_all(fd, text, strlen(text));
    klog_printf("ps5term-recv: %s", text);
}

/* Reads one '\n'-terminated line byte by byte so no payload bytes are consumed. */
static int read_line(int fd, char *line, size_t size)
{
    for (size_t used = 0; used + 1 < size;)
    {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        if (c == '\n')
        {
            line[used] = '\0';
            return 0;
        }
        line[used++] = c;
    }
    return -1;
}

/* Root-relative, no empty, "." or ".." components, inside one allowed root. */
static const char *allowed_root(const char *path)
{
    if (strstr(path, "//") || strstr(path, "/./") || strstr(path, "/../"))
        return NULL;
    size_t length = strlen(path);
    if (length == 0 || path[length - 1] == '/' || (length >= 3 && !strcmp(path + length - 3, "/..")) ||
        (length >= 2 && !strcmp(path + length - 2, "/.")))
        return NULL;
    for (size_t i = 0; i < sizeof allowed_roots / sizeof *allowed_roots; i++)
    {
        size_t root = strlen(allowed_roots[i]);
        if (length > root && !strncmp(path, allowed_roots[i], root))
            return allowed_roots[i];
    }
    return NULL;
}

/* Creates the root itself (only its last component) and every directory below it. */
static int make_parents(const char *path, const char *root)
{
    char buffer[1024];
    if (strlen(path) >= sizeof buffer)
        return -1;
    strcpy(buffer, path);
    size_t root_length = strlen(root);
    for (char *slash = buffer + root_length - 1; slash; slash = strchr(slash + 1, '/'))
    {
        *slash = '\0';
        if (mkdir(buffer, 0755) && errno != EEXIST)
            return -1;
        *slash = '/';
    }
    return 0;
}

static int receive(int client)
{
    char path[1024], size_text[32];
    if (read_line(client, path, sizeof path) || read_line(client, size_text, sizeof size_text))
    {
        reply(client, "ERR header\n");
        return 1;
    }
    char *end = NULL;
    long long size = strtoll(size_text, &end, 10);
    if (!*size_text || *end || size < 0 || size > MAX_FILE)
    {
        reply(client, "ERR size\n");
        return 1;
    }
    const char *root = allowed_root(path);
    if (!root)
    {
        reply(client, "ERR path\n");
        return 1;
    }
    char partial[1100];
    snprintf(partial, sizeof partial, "%s.partial", path);
    if (make_parents(path, root))
    {
        reply(client, "ERR mkdir\n");
        return 1;
    }
    int fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        reply(client, "ERR open\n");
        return 1;
    }
    Sha256 hash;
    sha256_init(&hash);
    static uint8_t buffer[256 * 1024];
    long long left = size;
    while (left > 0)
    {
        size_t want = left < (long long)sizeof buffer ? (size_t)left : sizeof buffer;
        ssize_t got = read(client, buffer, want);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0 || write_all(fd, buffer, (size_t)got))
        {
            close(fd);
            unlink(partial);
            reply(client, "ERR transfer\n");
            return 1;
        }
        sha256_update(&hash, buffer, (size_t)got);
        left -= got;
    }
    if (fsync(fd) || close(fd) || rename(partial, path))
    {
        unlink(partial);
        reply(client, "ERR commit\n");
        return 1;
    }
    uint8_t digest[32];
    sha256_final(&hash, digest);
    char text[80] = "OK ";
    for (int i = 0; i < 32; i++)
        snprintf(text + 3 + i * 2, 3, "%02x", digest[i]);
    strcat(text, "\n");
    reply(client, text);
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    alarm(300);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(RECV_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof address) || listen(server, 1))
    {
        klog_printf("ps5term-recv: cannot listen on %d: %s\n", RECV_PORT, strerror(errno));
        return 1;
    }
    klog_printf("ps5term-recv: listening on %d\n", RECV_PORT);
    int client = accept(server, NULL, NULL);
    close(server);
    if (client < 0)
        return 1;
    int status = receive(client);
    close(client);
    return status;
}
