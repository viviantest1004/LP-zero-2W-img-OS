/* head - the first few lines.
 *
 *   head [-n count] [file]...      (default 10)
 *   head -5 file                   the short form works too
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static void head_fd(int fd, long limit)
{
    char line[8192];
    for (long i = 0; i < limit; i++) {
        long n = readline(fd, line, sizeof(line));
        if (n < 0)
            break;
        printf("%s\n", line);
    }
}

int main(int argc, char **argv)
{
    long limit = 10;
    int  files = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            limit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: head [-n count] [file]...\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            limit = strtol(argv[i] + 1, NULL, 10);
        } else {
            files++;
        }
    }

    if (files == 0) {
        head_fd(STDIN_FILENO, limit);
        return 0;
    }

    int rc = 0, shown = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-n") == 0) i++;
            continue;
        }
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "head: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        if (files > 1)
            printf("%s==> %s <==\n", shown ? "\n" : "", argv[i]);
        head_fd((int)fd, limit);
        lp_close((int)fd);
        shown++;
    }
    return rc;
}
