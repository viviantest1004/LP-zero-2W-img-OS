/* mount - mount and unmount filesystems.
 *
 *   mount                        list what is mounted (/proc/mounts)
 *   mount <device> <dir>         guess the filesystem type
 *   mount -t <type> <device> <dir>
 *   mount -o ro ...              read only
 *   umount <dir>                 (when argv[0] is umount)
 *
 * With no type given we try the list below in order. The kernel returns
 * EINVAL for a type that does not fit, so trying them one by one works.
 * Our kernel only carries a handful of filesystems, so this is quick. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

static const char *AUTO_TYPES[] = { "ext4", "vfat", "ext2", NULL };

static int show_mounts(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "mount: cannot read /proc/mounts"
                " (is /proc mounted?)\n");
        return 1;
    }

    char buf[4096];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        lp_write(STDOUT_FILENO, buf, (size_t)n);
    }
    lp_close((int)fd);
    return 0;
}

static int do_umount(const char *target)
{
    long rc = sys_call2(SYS_umount2, (long)target, 0);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "umount: %s: failed (%ld)\n", target, -rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* argv[0] decides: the same binary is installed under both names. */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (strcmp(base, "umount") == 0) {
        if (argc < 2) {
            dprintf(STDERR_FILENO, "usage: umount <dir>\n");
            return 2;
        }
        return do_umount(argv[1]);
    }

    if (argc == 1)
        return show_mounts();

    const char *type   = NULL;
    unsigned long flags = 0;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            i++;
            if (strstr(argv[i], "bind"))   flags |= MS_BIND;
            if (strstr(argv[i], "ro"))     flags |= MS_RDONLY;
            if (strstr(argv[i], "noexec")) flags |= MS_NOEXEC;
            if (strstr(argv[i], "nosuid")) flags |= MS_NOSUID;
        } else if (nargs < 2) {
            args[nargs++] = argv[i];
        }
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO,
                "usage: mount [-t type] [-o ro|bind] <source> <dir>\n");
        return 2;
    }

    const char *src = args[0], *dst = args[1];

    /* A bind mount has no filesystem type. It makes a directory visible at
     * a second place, so it keeps the type of the original. */
    if (flags & MS_BIND) {
        long rc = lp_mount(src, dst, NULL, flags, NULL);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: bind %s -> %s: failed (%ld)\n",
                    src, dst, -rc);
            return 1;
        }
        return 0;
    }

    if (type) {
        long rc = lp_mount(src, dst, type, flags, NULL);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: %s -> %s (%s): failed (%ld)\n",
                    src, dst, type, -rc);
            return 1;
        }
        return 0;
    }

    /* Guess the type. */
    long last = -1;
    for (int i = 0; AUTO_TYPES[i]; i++) {
        long rc = lp_mount(src, dst, AUTO_TYPES[i], flags, NULL);
        if (rc == 0) {
            printf("mount: %s -> %s (%s)\n", src, dst, AUTO_TYPES[i]);
            return 0;
        }
        last = rc;
    }

    dprintf(STDERR_FILENO, "mount: %s -> %s: no filesystem fits (%ld)\n",
            src, dst, -last);
    return 1;
}
