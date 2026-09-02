/* fsck - check and repair the data partition.
 *
 *   fsck [-f] <device>...
 *
 * Tries each device in turn and checks the first one that exists, which
 * is how everything else in /etc/rc finds the disk: the name depends on
 * whether this is an SD card, a USB disk, NVMe or a virtual machine.
 *
 * ── Why this exists ──
 * /data is mounted errors=remount-ro, so ext4 corruption is noticed and
 * the filesystem stops taking writes. That is the right first move and
 * it is also the whole of what the system could do about it: there was
 * no way to repair it without pulling the card and finding another
 * computer. On a board sitting headless on a shelf, that is the same as
 * unrecoverable.
 *
 * ── Where e2fsck lives ──
 * On the boot partition, not in this filesystem image.
 *
 *   Not on /data     that is the thing being repaired
 *   Not in the image the initramfs is unpacked into RAM and stays there
 *                    for the life of the machine. 1.4MB of memory,
 *                    permanently, for a program that runs for a
 *                    fraction of a second at boot and does nothing at
 *                    all when the filesystem is clean
 *   On /boot         costs no memory, and it is read-only mounted, so
 *                    the tool cannot be damaged by the failure it is
 *                    there to fix
 *
 * ── Preen mode ──
 * -p: fix what can be fixed without asking, and do nothing at all when
 * the filesystem is already clean. A clean check reads the superblock
 * and exits, so running this on every boot costs milliseconds. Without
 * -p, e2fsck asks questions, and there is nobody at the console at boot
 * to answer them.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define E2FSCK "/boot/e2fsck"

/* e2fsck's exit codes are a bitmask:
 *   0  clean
 *   1  errors were corrected
 *   2  corrected, and the system should be rebooted
 *   4  errors are left uncorrected
 *   8  an operational error
 *  16  a usage error
 * Anything with 4 set means the filesystem is still broken. */
#define FSCK_CORRECTED   1
#define FSCK_REBOOT      2
#define FSCK_UNCORRECTED 4

static int run_e2fsck(const char *dev, bool force)
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        char *argv[5];
        int   n = 0;
        argv[n++] = (char *)E2FSCK;
        argv[n++] = (char *)"-p";      /* fix what is safe, ask nothing */
        if (force)
            argv[n++] = (char *)"-f";  /* check even when marked clean */
        argv[n++] = (char *)dev;
        argv[n]   = NULL;

        char *envp[] = { NULL };
        lp_execve(E2FSCK, argv, envp);
        lp_exit(127);
    }

    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    bool force = false;
    int  first = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            force = true;
            first = i + 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: fsck [-f] <device>...\n");
            printf("  checks the first device in the list that exists\n");
            printf("  -f  check even when the filesystem says it is clean\n");
            printf("\ne2fsck itself lives on the boot partition (%s),\n",
                   E2FSCK);
            printf("so mount /boot before this runs.\n");
            return 0;
        }
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: fsck [-f] <device>...\n");
        return 2;
    }

    if (!lp_exists(E2FSCK)) {
        /* Not an error worth stopping the boot for: an image built
         * without it still works, it just cannot repair itself. */
        printf("fsck: %s is not there - skipping the check\n", E2FSCK);
        return 0;
    }

    for (int i = first; i < argc; i++) {
        if (!lp_exists(argv[i]))
            continue;                  /* a different kind of disk */

        int rc = run_e2fsck(argv[i], force);

        if (rc == 0)
            return 0;                  /* clean, and quiet about it */

        if (rc < 0) {
            dprintf(STDERR_FILENO, "fsck: could not run %s\n", E2FSCK);
            return 1;
        }

        if (rc & FSCK_UNCORRECTED) {
            dprintf(STDERR_FILENO,
                    "fsck: ** %s is damaged and could not be repaired\n"
                    "fsck:    automatically. Run this by hand to answer\n"
                    "fsck:    its questions:  %s -f %s\n",
                    argv[i], E2FSCK, argv[i]);
            return 1;
        }

        if (rc & FSCK_CORRECTED)
            printf("fsck: %s had errors, and they were repaired\n", argv[i]);

        if (rc & FSCK_REBOOT)
            printf("fsck:   the repair wants a reboot to settle\n");

        return 0;
    }

    /* None of the named devices exist. Normal: the list covers SD, USB,
     * NVMe and virtio, and only one of them is ever there. */
    return 0;
}
