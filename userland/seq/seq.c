/* seq - print a run of numbers.
 *
 *   seq last
 *   seq first last
 *   seq first step last
 *   seq -s , 1 10        put something other than a newline between them
 *   seq -w 1 10          pad with zeros so the widths line up
 *
 * Integers only. This system has no floating point in its libc, and a
 * `seq 0 0.1 1` that silently counted in whole numbers would be worse
 * than one that says it cannot.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static int digits(long v)
{
    int n = (v < 0) ? 2 : 1;
    if (v < 0) v = -v;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static bool number(const char *s, long *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    const char *sep = "\n";
    bool pad = false;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) sep = argv[++i];
        else if (strcmp(argv[i], "-w") == 0) pad = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: seq [-s sep] [-w] [first [step]] last\n");
            printf("  whole numbers only\n");
            return 0;
        }
        else break;
    }

    long first = 1, step = 1, last = 0;
    int n = argc - i;
    if (n == 1)      { if (!number(argv[i], &last)) goto bad; }
    else if (n == 2) { if (!number(argv[i], &first) ||
                           !number(argv[i+1], &last)) goto bad; }
    else if (n == 3) { if (!number(argv[i], &first) ||
                           !number(argv[i+1], &step) ||
                           !number(argv[i+2], &last)) goto bad; }
    else goto bad;

    if (step == 0) {
        dprintf(STDERR_FILENO, "seq: a step of 0 never gets there\n");
        return 1;
    }

    int width = 1;
    if (pad) {
        int a = digits(first), b = digits(last);
        width = (a > b) ? a : b;
    }

    bool any = false;
    for (long v = first; (step > 0) ? (v <= last) : (v >= last); v += step) {
        if (any) fputs(sep, STDOUT_FILENO);
        if (pad) printf("%0*ld", width, v);
        else     printf("%ld", v);
        any = true;
    }
    if (any) fputs("\n", STDOUT_FILENO);
    return 0;

bad:
    dprintf(STDERR_FILENO, "usage: seq [-s sep] [-w] [first [step]] last\n");
    return 2;
}
