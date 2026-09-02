/* tail - the last few lines.
 *
 *   tail [-n count] [file]...      (default 10)
 *   tail -f file                   keep watching as it grows
 *
 * The last N lines cannot be known until the end of the file, and the
 * input may be a pipe that cannot be rewound. So we keep the last N in a
 * ring and print them when the input runs out. That bounds the memory at
 * N lines regardless of how big the file is.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_KEEP  200
#define LINE_MAX  1024

static char ring[MAX_KEEP][LINE_MAX];

static void tail_fd(int fd, long limit)
{
    if (limit > MAX_KEEP) limit = MAX_KEEP;
    if (limit < 1)        limit = 1;

    long count = 0;              /* lines seen in total */
    char line[LINE_MAX];

    for (;;) {
        long n = readline(fd, line, sizeof(line));
        if (n < 0)
            break;
        strlcpy(ring[count % limit], line, LINE_MAX);
        count++;
    }

    long start = (count > limit) ? count - limit : 0;
    for (long i = start; i < count; i++)
        printf("%s\n", ring[i % limit]);
}

/* -f: print what arrives from here on. There is no inotify in this
 * kernel build, so this polls - a second is far below what anyone
 * watching a log can notice, and costs nothing while nothing happens. */
static void follow(int fd)
{
    char buf[4096];
    for (;;) {
        long n = lp_read(fd, buf, sizeof(buf));
        if (n > 0)
            lp_write(STDOUT_FILENO, buf, (size_t)n);
        else
            lp_sleep_ms(1000);
    }
}

int main(int argc, char **argv)
{
    long limit = 10;
    bool watch = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            limit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-f") == 0) {
            watch = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: tail [-n count] [-f] [file]\n");
            printf("  -f  keep printing as the file grows\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            limit = strtol(argv[i] + 1, NULL, 10);
        } else if (!file) {
            file = argv[i];
        }
    }

    int fd = STDIN_FILENO;
    if (file) {
        long f = lp_open(file, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "tail: %s: cannot open\n", file);
            return 1;
        }
        fd = (int)f;
    }

    tail_fd(fd, limit);

    if (watch)
        follow(fd);                 /* never returns; Ctrl-C stops it */

    if (fd != STDIN_FILENO)
        lp_close(fd);
    return 0;
}
