/* rm - remove files and directories.
 *
 *   rm <path>...
 *   rm -r <path>...     including directories
 *   rm -f <path>...     a missing file is not an error
 *
 * This cannot be undone, so there are two guards against a slip:
 *   - "/" is always refused
 *   - removing a directory without -r is refused
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define ENOENT 2

static bool recursive = false;
static bool force     = false;
static int  failures  = 0;

static void oops(const char *what, const char *path, long rc)
{
    if (force && rc == -ENOENT)
        return;
    dprintf(STDERR_FILENO, "rm: %s: %s (%ld)\n", path, what, -rc);
    failures = 1;
}

static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlcpy(out, dir, cap);
    if (n >= cap) return false;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
        out[n] = '\0';
    }
    return strlcat(out, name, cap) < cap;
}

static int remove_any(const char *path);

static int remove_dir(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { oops("cannot open", path, fd); return 1; }

    /* Keep the buffer on the stack. As a static, recursion would overwrite
     * the parent's listing and rmdir would fail on a non-empty directory. */
    char dbuf[8192];
    int  rc = 0;

    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { oops("read failed", path, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[512];
            if (!join(child, sizeof(child), path, name)) {
                dprintf(STDERR_FILENO, "rm: path too long: %s\n", name);
                rc = 1;
                continue;
            }
            rc |= remove_any(child);
        }
    }

    lp_close((int)fd);

    /* rmdir only works on an empty directory, so anything that failed
     * above makes this fail too. */
    long r = lp_rmdir(path);
    if (r < 0) { oops("cannot remove", path, r); return 1; }
    return rc;
}

static int remove_any(const char *path)
{
    lp_stat_t st;
    long r = lp_stat(path, &st, false);      /* remove the link, not its target */
    if (r < 0) { oops("no such file", path, r); return force ? 0 : 1; }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        if (!recursive) {
            dprintf(STDERR_FILENO, "rm: %s is a directory (use -r)\n", path);
            failures = 1;
            return 1;
        }
        return remove_dir(path);
    }

    r = lp_unlink(path);
    if (r < 0) { oops("cannot remove", path, r); return force ? 0 : 1; }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc; first++) {
        if (strcmp(argv[first], "-r") == 0 || strcmp(argv[first], "-R") == 0)
            recursive = true;
        else if (strcmp(argv[first], "-f") == 0)
            force = true;
        else if (strcmp(argv[first], "-rf") == 0 || strcmp(argv[first], "-fr") == 0)
            recursive = force = true;
        else
            break;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: rm [-r] [-f] <path>...\n");
        return force ? 0 : 2;
    }

    for (int i = first; i < argc; i++) {
        /* Removing the root leaves no way back, so it is always refused.
         * Not just "/" but anything that resolves to it, like "//" or "/.". */
        const char *p = argv[i];
        bool only_root = true;
        for (const char *c = p; *c; c++)
            if (*c != '/' && *c != '.') { only_root = false; break; }
        if (only_root && p[0] == '/') {
            dprintf(STDERR_FILENO, "rm: refusing to remove the root (%s)\n", p);
            failures = 1;
            continue;
        }
        remove_any(p);
    }
    return failures;
}
