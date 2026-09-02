/* ln - make another name for a file.
 *
 *   ln -s <target> <name>     a symbolic link
 *   ln <file> <name>          a hard link
 *
 * A symbolic link is a small file holding a path; a hard link is a
 * second directory entry for the same data. The symbolic one is what
 * people mean, and it is the one that works across filesystems - which
 * matters here, where /data and / are different filesystems.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

#define AT_FDCWD (-100)

int main(int argc, char **argv)
{
    bool symbolic = false;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) symbolic = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: ln [-s] <target> <name>\n");
            printf("  -s  a symbolic link (what you usually want)\n");
            return 0;
        }
        else if (nargs < 2) args[nargs++] = argv[i];
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO, "usage: ln [-s] <target> <name>\n");
        return 2;
    }

    long r;
    if (symbolic) {
        r = lp_symlink(args[0], args[1]);
    } else {
        /* linkat(AT_FDCWD, old, AT_FDCWD, new, 0) */
        r = sys_call5(SYS_linkat, AT_FDCWD, (long)args[0],
                      AT_FDCWD, (long)args[1], 0);
    }

    if (r < 0) {
        dprintf(STDERR_FILENO, "ln: %s -> %s: failed (%ld)%s\n",
                args[1], args[0], -r,
                (!symbolic && -r == 18) ? " - a hard link cannot cross filesystems, try -s" : "");
        return 1;
    }
    return 0;
}
