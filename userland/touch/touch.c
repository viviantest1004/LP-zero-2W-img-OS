/* touch - create an empty file.
 *
 *   touch [-c] <file>...
 *
 * What is missing, and why: GNU's touch also sets the timestamps, and
 * this one does not. The kernel call for that is utimensat(2) and libc
 * has no wrapper for it, so rather than reach round the back and pick a
 * syscall number out of the air, the file is created and the times are
 * left as the filesystem set them. `touch existing-file` therefore
 * succeeds and changes nothing. Nothing on this system decides anything
 * from an mtime - there is no make - so the gap costs nothing here, but
 * it is a real difference from Ubuntu and it is not hidden.
 *
 * -a, -m, -d, -r and -t are all about times, so none of them are here
 * either; typing one gets the ordinary "invalid option" rather than a
 * flag that silently does nothing.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void usage(int fd)
{
    dprintf(fd, "Usage: touch [OPTION]... FILE...\n"
                "Create each FILE that does not already exist.\n\n"
                "  -c, --no-create   do not create any files\n"
                "      --help     display this help and exit\n\n"
                "The timestamps of an existing FILE are left alone: this system has no\n"
                "way to set them.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "no-create", 0, 'c' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool no_create = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "c", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'c': no_create = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("touch", &g); return 1;
        }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "touch: missing file operand\n"
                               "Try 'touch --help' for more information.\n");
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        if (lp_exists(argv[i]))
            continue;
        if (no_create)
            continue;
        /* No O_EXCL. If someone else created it between the check and here,
         * the file exists, which is what we wanted anyway. */
        long fd = lp_open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            lp_diag("touch", "cannot touch", NULL, "cannot create",
                    argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        lp_close((int)fd);
    }
    return rc;
}
