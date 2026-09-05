/* cmp - are these two files the same, and if not, where do they differ.
 *
 *   cmp [-s] [-l] a b
 *
 *   -s  say nothing, just set the exit status
 *   -l  every differing byte, not only the first
 *
 * `sha256sum` answers "are these the same" for whole files. This answers
 * "where did they start to differ", which is what you want when a
 * download is truncated or a card is going bad: the offset tells you
 * how far the good part goes.
 *
 * Exit status: 0 the same, 1 different, 2 something went wrong. Scripts
 * rely on that split, so a missing file must not look like a difference.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    bool silent = false, list = false;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) silent = true;
        else if (strcmp(argv[i], "-l") == 0) list = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: cmp [-s] [-l] a b\n");
            printf("  -s  no output, status only   -l  every difference\n");
            printf("  status: 0 same, 1 different, 2 error\n");
            return 0;
        }
        else break;
    }

    if (argc - i != 2) {
        dprintf(STDERR_FILENO, "usage: cmp [-s] [-l] a b\n");
        return 2;
    }

    const char *na = argv[i], *nb = argv[i + 1];
    long fa = lp_open(na, O_RDONLY, 0);
    if (fa < 0) {
        dprintf(STDERR_FILENO, "cmp: %s: cannot open\n", na);
        return 2;
    }
    long fb = lp_open(nb, O_RDONLY, 0);
    if (fb < 0) {
        dprintf(STDERR_FILENO, "cmp: %s: cannot open\n", nb);
        lp_close((int)fa);
        return 2;
    }

    unsigned char ba[8192], bb[8192];
    long long off = 0, line = 1;
    int rc = 0;

    for (;;) {
        long na_read = lp_read((int)fa, ba, sizeof ba);
        long nb_read = lp_read((int)fb, bb, sizeof bb);
        if (na_read < 0 || nb_read < 0) { rc = 2; break; }

        long n = (na_read < nb_read) ? na_read : nb_read;
        for (long k = 0; k < n; k++) {
            if (ba[k] != bb[k]) {
                rc = 1;
                if (silent) goto done;
                if (list)
                    printf("%lld %o %o\n", off + k + 1,
                           ba[k], bb[k]);
                else {
                    printf("%s %s differ: byte %lld, line %lld\n",
                           na, nb, off + k + 1, line);
                    goto done;
                }
            }
            if (ba[k] == '\n') line++;
        }
        off += n;

        if (na_read != nb_read) {
            /* One ran out first. That is a difference, and saying which
             * one ended saves the next question. */
            rc = 1;
            if (!silent)
                dprintf(STDERR_FILENO, "cmp: EOF on %s after byte %lld\n",
                        (na_read < nb_read) ? na : nb, off);
            break;
        }
        if (na_read == 0) break;      /* both ended together */
    }

done:
    lp_close((int)fa);
    lp_close((int)fb);
    return rc;
}
