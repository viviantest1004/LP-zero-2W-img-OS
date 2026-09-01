/* cp - copy files.
 *
 *   cp <source> <dest>
 *   cp <source>... <directory>
 *   cp -r <source>... <dest>    including directories
 *   cp -n <source> <dest>       leave the destination alone if it exists (success)
 *   cp -q <source> <dest>       stay quiet when the source is missing
 *                               (the exit status still says it failed)
 *
 * -n and -q exist for the boot script (/etc/rc). Our shell has neither if
 * nor test, so this is the only way to say "skip it if it is already there,
 * create it if it is not".
 *
 * Permissions follow the source. Owner and timestamps do not - there is
 * only one user on this system, so they would mean nothing.
 *
 * Copying a file onto itself is refused. Opening it would truncate the
 * source before reading it, and the contents would be gone.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE  32768

/* linux_dirent64 offsets. We read by offset rather than declaring a
 * struct, so nothing depends on the compiler's padding. (Same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19
#define EEXIST    17

#define ENOENT 2

static bool recursive  = false;
static bool no_clobber = false;
static bool quiet      = false;
static int  failures   = 0;

static void oops(const char *what, const char *path, long rc)
{
    if (quiet && rc == -ENOENT)
        return;                 /* stay quiet, but still count it as a failure */
    dprintf(STDERR_FILENO, "cp: %s: %s (%ld)\n", path, what, -rc);
    failures = 1;
}

/* Last element of a path. "/a/b/c" -> "c", "/a/b/" -> "b" */
static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
}

/* Join dir and name. false if it would not fit. */
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

static int copy_file(const char *src, const char *dst)
{
    if (no_clobber && lp_exists(dst))
        return 0;               /* already there - leaving it alone is success */

    lp_stat_t st;
    long r = lp_stat(src, &st, true);
    if (r < 0) {
        oops("cannot read", src, r);
        if (quiet) failures = 1;
        return 1;
    }

    /* Copying a file onto itself would lose it. Catch it first.
     * The right comparison is device+inode, not the path string, but our
     * stat does not carry those fields. A string compare catches the
     * common mistake. */
    if (strcmp(src, dst) == 0) {
        dprintf(STDERR_FILENO, "cp: %s and the destination are the same file\n", src);
        failures = 1;
        return 1;
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) { oops("cannot open", src, in); return 1; }

    /* Create it with the source's permissions, so an executable stays one. */
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777);
    if (out < 0) { lp_close((int)in); oops("cannot create", dst, out); return 1; }

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { oops("read failed", src, n); rc = 1; break; }

        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) { oops("write failed", dst, w); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }

    lp_close((int)in);
    lp_close((int)out);

    /* umask trims the mode O_CREAT asked for. Set it exactly. */
    if (rc == 0)
        lp_chmod(dst, st.mode & 07777);
    return rc;
}

static int copy_any(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst)
{
    if (!recursive) {
        dprintf(STDERR_FILENO, "cp: %s is a directory (use -r)\n", src);
        failures = 1;
        return 1;
    }

    lp_stat_t st;
    if (lp_stat(src, &st, true) < 0) return 1;

    long r = lp_mkdir(dst, st.mode & 07777);
    if (r < 0 && r != -EEXIST) { oops("cannot create", dst, r); return 1; }

    long fd = lp_open(src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { oops("cannot open", src, fd); return 1; }

    /* Keep the buffer on the stack. As a static, a recursive call would
     * overwrite the parent's and entries would vanish silently. 8KB a level. */
    char dbuf[8192];
    int  rc = 0;

    /* getdents does not hand over everything at once. Loop until it returns 0. */
    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { oops("read failed", src, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char s[512], t[512];
            if (!join(s, sizeof(s), src, name) ||
                !join(t, sizeof(t), dst, name)) {
                dprintf(STDERR_FILENO, "cp: path too long: %s\n", name);
                rc = 1;
                continue;
            }
            rc |= copy_any(s, t);
        }
    }

    lp_close((int)fd);
    return rc;
}

static int copy_any(const char *src, const char *dst)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, false);        /* look at the link, not its target */
    if (r < 0) { oops("cannot read", src, r); return 1; }

    if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
        char target[512];
        long n = lp_readlink(src, target, sizeof(target) - 1);
        if (n < 0) { oops("cannot read the link", src, n); return 1; }
        target[n] = '\0';
        lp_unlink(dst);                        /* replace any existing entry */
        long lr = lp_symlink(target, dst);
        if (lr < 0) { oops("cannot create the link", dst, lr); return 1; }
        return 0;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR)
        return copy_dir(src, dst);

    return copy_file(src, dst);
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc; first++) {
        const char *a = argv[first];
        if (strcmp(a, "-r") == 0 || strcmp(a, "-R") == 0) recursive  = true;
        else if (strcmp(a, "-n") == 0)                    no_clobber = true;
        else if (strcmp(a, "-q") == 0)                    quiet      = true;
        else break;
    }

    if (argc - first < 2) {
        dprintf(STDERR_FILENO, "usage: cp [-r] [-n] [-q] <source>... <dest>\n");
        return 2;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);
    int  nsrc = argc - 1 - first;

    /* With several sources the destination must be a directory, or each
     * would overwrite the last and only one would survive. */
    if (nsrc > 1 && !dst_is_dir) {
        dprintf(STDERR_FILENO, "cp: with several sources the destination must be a directory\n");
        return 2;
    }

    for (int i = first; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                dprintf(STDERR_FILENO, "cp: path too long\n");
                failures = 1;
                continue;
            }
            copy_any(argv[i], full);
        } else {
            copy_any(argv[i], dst);
        }
    }
    return failures;
}
