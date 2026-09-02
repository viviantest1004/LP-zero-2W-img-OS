/* uniq - collapse repeated lines.
 *
 *   uniq [-c] [-d] [file]
 *
 *   -c  count each run    -d  only the lines that repeat
 *
 * Only lines that are next to each other count as repeats, which is why
 * this is nearly always used after sort:
 *
 *   cat log | sort | uniq -c | sort -rn
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    bool count = false, dups_only = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) count = true;
        else if (strcmp(argv[i], "-d") == 0) dups_only = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: uniq [-c] [-d] [file]\n");
            printf("  -c  count each run   -d  only what repeats\n");
            printf("  repeats have to be adjacent - sort first\n");
            return 0;
        }
        else if (!file) file = argv[i];
    }

    int fd = STDIN_FILENO;
    if (file) {
        long f = lp_open(file, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "uniq: %s: cannot open\n", file);
            return 1;
        }
        fd = (int)f;
    }

    char cur[4096], prev[4096];
    long run = 0;
    bool have_prev = false;

    for (;;) {
        long n = readline(fd, cur, sizeof(cur));

        if (n >= 0 && have_prev && strcmp(cur, prev) == 0) {
            run++;
            continue;
        }

        if (have_prev) {
            if (!dups_only || run > 1) {
                if (count) printf("%7ld %s\n", run, prev);
                else       printf("%s\n", prev);
            }
        }

        if (n < 0)
            break;

        strlcpy(prev, cur, sizeof(prev));
        have_prev = true;
        run = 1;
    }

    if (fd != STDIN_FILENO)
        lp_close(fd);
    return 0;
}
