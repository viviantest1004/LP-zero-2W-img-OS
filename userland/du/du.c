/* du - how much space a directory takes.
 *
 *   du [-s] [-b] [path]...
 *
 *   -s  the total only, no line per subdirectory
 *   -b  count bytes rather than MB
 *
 * This adds up file sizes, not the blocks on disk. The two differ - a
 * file smaller than a block still occupies one, and a sparse file
 * occupies less than its size - but "how much is in here" is nearly
 * always the question being asked, and it is the one that matches what
 * copying it somewhere else would cost.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

static bool summary_only = false;
static bool in_bytes     = false;

static void show(const char *path, u64 bytes)
{
    if (in_bytes)
        printf("%12llu  %s\n", (unsigned long long)bytes, path);
    else
        printf("%8llu MB  %s\n",
               (unsigned long long)(bytes / 1048576ULL), path);
}

/* Returns the total under `path`, printing a line per directory unless
 * we were asked for the summary alone. */
static u64 walk(const char *path, int depth)
{
    u64 total = 0;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        lp_stat_t st;
        if (lp_stat(path, &st, true) == 0)
            total = st.size;
        return total;
    }

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[1024];
            snprintf(child, sizeof(child), "%s%s%s",
                     path, strcmp(path, "/") == 0 ? "" : "/", name);

            if (type == DT_DIR) {
                total += walk(child, depth + 1);
            } else {
                lp_stat_t st;
                /* The link itself, not what it points at - otherwise a
                 * symlink into a big tree gets counted twice. */
                if (lp_stat(child, &st, false) == 0)
                    total += st.size;
            }
        }
    }
    lp_close((int)fd);

    if (!summary_only)
        show(path, total);
    return total;
}

int main(int argc, char **argv)
{
    int paths = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) summary_only = true;
        else if (strcmp(argv[i], "-b") == 0) in_bytes = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: du [-s] [-b] [path]...\n");
            printf("  -s  the total only\n");
            printf("  -b  bytes instead of MB\n");
            return 0;
        }
        else paths++;
    }

    if (paths == 0) {
        u64 t = walk(".", 0);
        if (summary_only)
            show(".", t);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        u64 t = walk(argv[i], 0);
        if (summary_only)
            show(argv[i], t);
    }
    return 0;
}
