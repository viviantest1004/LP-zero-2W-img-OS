/* bootcount - break a reboot loop.
 *
 * The watchdog reboots a board that has stopped answering. That is the
 * right thing to do exactly once. If whatever wedged the machine is in
 * the startup script, the watchdog will do it again, and again, and the
 * board spends the rest of its life rebooting - with no window to log in
 * and fix the file that is causing it. A headless board on a shelf
 * cannot be rescued from that state without pulling the card.
 *
 * So we count. Every boot bumps a number on the data partition. guard
 * clears it once the system has stayed up for five minutes, which is the
 * definition of a boot that worked. The number therefore only ever grows
 * when boots keep failing, and when it gets to LIMIT we say so and exit
 * non-zero. /etc/rc uses that to skip /data/rc.local - the one part of
 * the boot the user can change, and so the one most likely to be at
 * fault.
 *
 * There is deliberately no clock in any of this. At this point in the
 * boot the time has been restored from a file at best, and a board that
 * has never reached the network has no idea what year it is.
 *
 *   exit 0   carry on
 *   exit 1   we are in a loop - skip the startup script
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

#define STATE_FILE  "/data/boot_count"
#define LIMIT       5      /* failed boots before we stop trusting rc.local */
#define CAP        99      /* do not let the number run away */

static int read_count(void)
{
    char buf[32];
    long n = proc_read(STATE_FILE, buf, sizeof(buf));
    if (n <= 0)
        return 0;                  /* first boot, or no data partition */
    long v = strtol(buf, NULL, 10);
    if (v < 0)   v = 0;
    if (v > CAP) v = CAP;
    return (int)v;
}

static bool write_count(int v)
{
    long fd = lp_open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;              /* read-only, or running from RAM */

    char buf[16];
    int  n = snprintf(buf, sizeof(buf), "%d\n", v);
    bool ok = lp_write((int)fd, buf, (size_t)n) == n;
    lp_close((int)fd);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        /* Clear it. This is what a boot that worked looks like. */
        write_count(0);
        return 0;
    }
    if (argc > 1) {
        printf("usage: bootcount [-c]\n");
        printf("  counts boots that did not last; -c clears the count\n");
        printf("  exits 1 once %d boots in a row have failed\n", LIMIT);
        return 2;
    }

    int count = read_count() + 1;

    /* If we cannot write it we cannot count, and a counter that never
     * moves would eventually be wrong in the dangerous direction - it
     * would keep saying "carry on". Say nothing and carry on anyway:
     * without a data partition there is no rc.local to skip either. */
    if (!write_count(count))
        return 0;

    if (count < LIMIT)
        return 0;

    printf("[boot] ** %d boots in a row did not last five minutes.\n", count);
    printf("[boot]    Skipping /data/rc.local - it is the usual cause.\n");
    printf("[boot]    Fix or delete it, then: bootcount -c\n");
    return 1;
}
