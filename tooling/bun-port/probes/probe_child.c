/* PS5Term - probe_child: started by probe_spawn; echoes argv[1] and $PROBE_CHILD.
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *env = getenv("PROBE_CHILD");
    char line[256];
    int size = snprintf(line, sizeof line, "child-ok %s PROBE_CHILD=%s\n", argc > 1 ? argv[1] : "-",
                        env ? env : "-");
    return write(STDOUT_FILENO, line, (size_t)size) == size ? 0 : 1;
}
