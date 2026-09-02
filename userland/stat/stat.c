/* stat - what a file is.
 *
 *   stat <path>...
 *
 * Size, type and permissions. ls -l shows the same things in one line
 * each; this spells them out, which is what you want when the question
 * is "why can I not run this" and the answer is a missing execute bit.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static const char *type_name(u32 mode)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR: return "directory";
    case LP_S_IFREG: return "file";
    case LP_S_IFLNK: return "symbolic link";
    case 0020000:    return "character device";
    case 0060000:    return "block device";
    case 0010000:    return "named pipe";
    case 0140000:    return "socket";
    default:         return "unknown";
    }
}

static void permissions(u32 mode, char *out)
{
    static const char *rwx[] = { "---", "--x", "-w-", "-wx",
                                 "r--", "r-x", "rw-", "rwx" };
    out[0] = '\0';
    strlcat(out, rwx[(mode >> 6) & 7], 10);
    strlcat(out, rwx[(mode >> 3) & 7], 10);
    strlcat(out, rwx[mode & 7], 10);
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: stat <path>...\n");
        return argc < 2 ? 2 : 0;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        lp_stat_t st;
        /* The link itself, not what it points at - otherwise a broken
         * symlink looks like a missing file. */
        if (lp_stat(argv[i], &st, false) < 0) {
            dprintf(STDERR_FILENO, "stat: %s: not there\n", argv[i]);
            rc = 1;
            continue;
        }

        char perm[10];
        permissions(st.mode, perm);

        printf("%s\n", argv[i]);
        printf("  type   %s\n", type_name(st.mode));
        printf("  mode   %s  (%04o)\n", perm, st.mode & 07777);

        if ((st.mode & LP_S_IFMT) == LP_S_IFREG) {
            if (st.size >= 1048576)
                printf("  size   %llu bytes  (%llu MB)\n",
                       (unsigned long long)st.size,
                       (unsigned long long)(st.size / 1048576));
            else if (st.size >= 1024)
                printf("  size   %llu bytes  (%llu KB)\n",
                       (unsigned long long)st.size,
                       (unsigned long long)(st.size / 1024));
            else
                printf("  size   %llu bytes\n", (unsigned long long)st.size);
        }

        if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
            char target[512];
            long n = lp_readlink(argv[i], target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = '\0';
                printf("  points %s\n", target);
            }
        }
        printf("\n");
    }
    return rc;
}
