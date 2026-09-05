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

/* Is this an existing directory? A symlink to one counts - following
 * it is what every other ln does. */
static bool is_directory(const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, true) < 0)
        return false;
    return (st.mode & 0170000) == 0040000;   /* S_IFDIR */
}

int main(int argc, char **argv)
{
    bool symbolic = false;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) symbolic = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: ln [-s] <target> <name|directory>\n");
            printf("  -s  a symbolic link (what you usually want)\n");
            printf("\nWhen the second argument is a directory, the link\n");
            printf("is made inside it under the target's own name.\n");
            return 0;
        }
        else if (nargs < 2) args[nargs++] = argv[i];
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO, "usage: ln [-s] <target> <name>\n");
        return 2;
    }

    /* ── "ln -s /some/file /a/directory/" ──
     *
     * Every other ln does this: when the second argument is a
     * directory, the link is made inside it under the target's own
     * name. Ours did not, so it created a link *called* /a/directory,
     * which either failed with EEXIST or - worse - quietly worked and
     * left a symlink where a directory was expected.
     *
     * /etc/rc needs it to link a whole glibc directory's worth of
     * libraries into /lib without a basename command to take them
     * apart. */
    char joined[512];
    const char *dest = args[1];

    if (is_directory(dest)) {
        const char *base = strrchr(args[0], '/');
        base = base ? base + 1 : args[0];

        /* A trailing slash on the destination would double up. */
        size_t len = strlen(dest);
        if (len > 0 && dest[len - 1] == '/')
            snprintf(joined, sizeof joined, "%s%s", dest, base);
        else
            snprintf(joined, sizeof joined, "%s/%s", dest, base);
        dest = joined;
    }

    long r;
    if (symbolic) {
        r = lp_symlink(args[0], dest);
    } else {
        /* linkat(AT_FDCWD, old, AT_FDCWD, new, 0) */
        r = sys_call5(SYS_linkat, AT_FDCWD, (long)args[0],
                      AT_FDCWD, (long)dest, 0);
    }

    if (r < 0) {
        dprintf(STDERR_FILENO, "ln: %s -> %s: failed (%ld)%s\n",
                dest, args[0], -r,
                (!symbolic && -r == 18) ? " - a hard link cannot cross filesystems, try -s" : "");
        return 1;
    }
    return 0;
}
