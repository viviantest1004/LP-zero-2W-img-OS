/* mv - move or rename files.
 *
 *   mv <source> <dest>
 *   mv <source>... <directory>
 *
 * Within one filesystem this is a single rename. No data moves - only the
 * directory entry changes - so it is instant whatever the file size, and a
 * power cut cannot leave a half-written file behind.
 *
 * Across filesystems rename fails with EXDEV - moving from /tmp (RAM) to
 * /data (SD card), say. Then we copy and remove the original instead.
 * We do that here rather than calling cp, because the original must not be
 * removed when the copy fails.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 32768
#define EXDEV    18

static int failures = 0;

static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
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

/* Crossing filesystems: copy, check, then remove. The order matters. */
static int move_across(const char *src, const char *dst)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, true);
    if (r < 0) {
        dprintf(STDERR_FILENO, "mv: %s: cannot read (%ld)\n", src, -r);
        return 1;
    }
    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        dprintf(STDERR_FILENO,
                "mv: %s: moving a directory across filesystems is not supported\n"
                "    (copy it with cp -r, then remove it with rm -r)\n", src);
        return 1;
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) {
        dprintf(STDERR_FILENO, "mv: %s: cannot open (%ld)\n", src, -in);
        return 1;
    }
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777);
    if (out < 0) {
        lp_close((int)in);
        dprintf(STDERR_FILENO, "mv: %s: cannot create (%ld)\n", dst, -out);
        return 1;
    }

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { dprintf(STDERR_FILENO, "mv: read failed (%ld)\n", -n); rc = 1; break; }
        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) { dprintf(STDERR_FILENO, "mv: write failed (%ld)\n", -w); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    lp_close((int)in);
    lp_close((int)out);

    if (rc != 0) {
        /* Do not leave a half-written destination. The original stays. */
        lp_unlink(dst);
        return 1;
    }

    lp_chmod(dst, st.mode & 07777);

    long u = lp_unlink(src);
    if (u < 0) {
        dprintf(STDERR_FILENO,
                "mv: copied to %s but could not remove the original (%ld)\n", dst, -u);
        return 1;
    }
    return 0;
}

static int move_one(const char *src, const char *dst)
{
    long r = lp_rename(src, dst);
    if (r == 0)
        return 0;
    if (r == -EXDEV)
        return move_across(src, dst);

    dprintf(STDERR_FILENO, "mv: %s -> %s: failed (%ld)\n", src, dst, -r);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: mv <source>... <dest>\n");
        return 2;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);

    if (argc - 1 > 2 && !dst_is_dir) {
        dprintf(STDERR_FILENO, "mv: with several sources the destination must be a directory\n");
        return 2;
    }

    for (int i = 1; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                dprintf(STDERR_FILENO, "mv: path too long\n");
                failures = 1;
                continue;
            }
            failures |= move_one(argv[i], full);
        } else {
            failures |= move_one(argv[i], dst);
        }
    }
    return failures;
}
