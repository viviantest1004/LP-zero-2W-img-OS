/* sleep - wait.
 *
 *   sleep <seconds>
 *   sleep 0.5           fractions are allowed
 *
 * Every system has this and ours did not, which makes a boot script
 * unable to wait for anything - there is no way to say "give the
 * network a moment" without it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: sleep <seconds>\n");
        return 2;
    }

    /* Parse seconds with up to three decimals, without floating point -
     * our printf has none and this does not need it. */
    const char *p = argv[1];
    long ms = 0;

    if (*p < '0' || *p > '9') {
        dprintf(STDERR_FILENO, "sleep: not a number: %s\n", argv[1]);
        return 2;
    }
    while (*p >= '0' && *p <= '9')
        ms = ms * 1000 + (*p++ - '0') * 1000;

    if (*p == '.') {
        p++;
        long scale = 100;
        while (*p >= '0' && *p <= '9' && scale > 0) {
            ms += (*p++ - '0') * scale;
            scale /= 10;
        }
        while (*p >= '0' && *p <= '9')      /* extra digits: ignore */
            p++;
    }

    if (*p != '\0') {
        dprintf(STDERR_FILENO, "sleep: not a number: %s\n", argv[1]);
        return 2;
    }

    lp_sleep_ms(ms);
    return 0;
}
