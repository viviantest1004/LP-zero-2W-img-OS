/* rev - reverse each line.
 *
 *   rev [file...]
 *
 * Characters, not bytes: reversing UTF-8 a byte at a time turns every
 * Hangul character into three broken ones, and this machine's console
 * speaks UTF-8.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void rev_line(const char *line)
{
    size_t len = strlen(line);
    char out[8192];
    size_t o = sizeof out - 1;
    out[o] = '\0';

    size_t i = 0;
    while (i < len) {
        int used = 0;
        utf8_decode(line + i, len - i, &used);
        if (used <= 0) used = 1;
        if (o < (size_t)used) break;     /* line longer than the buffer */
        o -= (size_t)used;
        memcpy(out + o, line + i, (size_t)used);
        i += (size_t)used;
    }
    printf("%s\n", out + o);
}

static int do_fd(int fd)
{
    char line[8192];
    while (readline(fd, line, sizeof line) >= 0)
        rev_line(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: rev [file...]\n");
        return 0;
    }
    if (argc < 2) return do_fd(STDIN_FILENO);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "rev: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        do_fd((int)fd);
        lp_close((int)fd);
    }
    return rc;
}
