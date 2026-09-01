/* cat - copy files, or standard input, to standard output. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 8192

/* Copy one fd to the end. 0 on success, 1 on failure. */
static int copy_fd(int fd, const char *label)
{
    static char buf[BUF_SIZE];

    for (;;) {
        long n = lp_read(fd, buf, sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0) {
            dprintf(STDERR_FILENO, "cat: %s: read failed (%ld)\n", label, -n);
            return 1;
        }

        /* write may take less than asked. Loop until it is all out. */
        long off = 0;
        while (off < n) {
            long w = lp_write(STDOUT_FILENO, buf + off, (size_t)(n - off));
            if (w <= 0) {
                dprintf(STDERR_FILENO, "cat: write failed (%ld)\n", -w);
                return 1;
            }
            off += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return copy_fd(STDIN_FILENO, "stdin");

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            rc |= copy_fd(STDIN_FILENO, "stdin");
            continue;
        }

        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: cannot open (%ld)\n",
                    argv[i], -fd);
            rc = 1;
            continue;
        }
        rc |= copy_fd((int)fd, argv[i]);
        lp_close((int)fd);
    }
    return rc;
}
