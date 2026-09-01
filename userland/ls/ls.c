/* ls - list a directory.
 *
 * getdents64 fills the buffer with variable-length records, back to back:
 *   struct linux_dirent64 {
 *       u64  d_ino;      offset 0
 *       s64  d_off;      offset 8
 *       u16  d_reclen;   offset 16   <- bytes to the next record
 *       u8   d_type;     offset 18
 *       char d_name[];   offset 19   NUL terminated
 *   };
 * We read by offset rather than declaring a struct, so nothing depends on
 * the compiler's padding rules. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF_SIZE       8192
#define DIRENT_RECLEN  16
#define DIRENT_TYPE    18
#define DIRENT_NAME    19

/* d_type values */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

static char type_suffix(u8 t)
{
    switch (t) {
    case DT_DIR:  return '/';
    case DT_LNK:  return '@';
    case DT_FIFO: return '|';
    case DT_SOCK: return '=';
    default:      return '\0';
    }
}

static int list_dir(const char *path, bool show_header, bool show_all)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: cannot open (%ld)\n", path, -fd);
        return 1;
    }

    if (show_header)
        printf("%s:\n", path);

    static char buf[BUF_SIZE];
    for (;;) {
        long n = sys_getdents(fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            dprintf(STDERR_FILENO, "ls: %s: read failed (%ld)\n", path, -n);
            lp_close((int)fd);
            return 1;
        }

        for (long off = 0; off < n; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;

            if (len == 0)       /* guard against a zero-length record looping forever */
                break;

            bool hidden = (name[0] == '.');
            if (show_all || !hidden) {
                char suffix = type_suffix(type);
                if (suffix) printf("%s%c\n", name, suffix);
                else        printf("%s\n", name);
            }
            off += len;
        }
    }

    lp_close((int)fd);
    return 0;
}

int main(int argc, char **argv)
{
    bool show_all = false;
    int  first_path = argc;
    int  npaths = 0;

    /* Split options from paths. */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (char *o = argv[i] + 1; *o; o++) {
                if (*o == 'a') show_all = true;
                else {
                    dprintf(STDERR_FILENO, "ls: unknown option -%c\n", *o);
                    return 2;
                }
            }
        } else {
            if (npaths == 0) first_path = i;
            npaths++;
        }
    }

    if (npaths == 0)
        return list_dir(".", false, show_all);

    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        rc |= list_dir(argv[i], npaths > 1, show_all);
    }
    return rc;
}
