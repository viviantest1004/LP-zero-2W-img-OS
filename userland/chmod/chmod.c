/* chmod - change what may be done with a file.
 *
 *   chmod 755 file...          the usual octal
 *   chmod +x file...           make it runnable
 *   chmod -w file...           make it read-only
 *
 * The symbolic form here is only the short one: a + or - and one or more
 * of r, w, x, applied to everybody. The full form (u=rw,go=r) needs the
 * current mode read back and each class handled apart, and on a
 * single-user machine the distinction never comes up.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* Turn "+x" or "-rw" into the bits to set or clear. */
static bool parse_symbolic(const char *spec, mode_t *bits, bool *adding)
{
    if (spec[0] != '+' && spec[0] != '-')
        return false;
    *adding = (spec[0] == '+');
    *bits = 0;

    for (const char *c = spec + 1; *c; c++) {
        switch (*c) {
        case 'r': *bits |= 0444; break;
        case 'w': *bits |= 0222; break;
        case 'x': *bits |= 0111; break;
        default:  return false;
        }
    }
    return *bits != 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "-h") == 0) {
        printf("usage: chmod <mode> <file>...\n");
        printf("  chmod 755 file    octal\n");
        printf("  chmod +x file     make it runnable\n");
        printf("  chmod -w file     make it read-only\n");
        return argc < 3 ? 2 : 0;
    }

    const char *spec = argv[1];
    mode_t      bits = 0;
    bool        adding = false;
    bool        symbolic = parse_symbolic(spec, &bits, &adding);
    mode_t      octal = 0;

    if (!symbolic) {
        /* Octal, and only octal: chmod 8 would otherwise quietly become
         * something nobody meant. */
        for (const char *c = spec; *c; c++) {
            if (*c < '0' || *c > '7') {
                dprintf(STDERR_FILENO,
                        "chmod: %s: not an octal mode, and not +x or -w\n",
                        spec);
                return 2;
            }
        }
        octal = (mode_t)strtol(spec, NULL, 8);
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        mode_t mode = octal;

        if (symbolic) {
            lp_stat_t st;
            if (lp_stat(argv[i], &st, true) < 0) {
                dprintf(STDERR_FILENO, "chmod: %s: not there\n", argv[i]);
                rc = 1;
                continue;
            }
            mode = (mode_t)(st.mode & 07777);
            mode = adding ? (mode | bits) : (mode & ~bits);
        }

        long r = lp_chmod(argv[i], mode);
        if (r < 0) {
            dprintf(STDERR_FILENO, "chmod: %s: failed (%ld)\n", argv[i], -r);
            rc = 1;
        }
    }
    return rc;
}
