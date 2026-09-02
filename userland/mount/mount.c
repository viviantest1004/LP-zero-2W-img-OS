/* mount - mount and unmount filesystems.
 *
 *   mount                        list what is mounted (/proc/mounts)
 *   mount <device> <dir>         guess the filesystem type
 *   mount -t <type> <device> <dir>
 *   mount -o ro,nosuid ...       flags, and filesystem options such as
 *                                size=64M or errors=remount-ro
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

/* Work out what is on a device by looking at it.
 *
 * Trying each filesystem in turn does work - the kernel says EINVAL for
 * one that does not fit - but every failed attempt is a line in the
 * kernel log, and those lines land on the screen during boot where they
 * read as something having gone wrong. Nothing has: we asked a question
 * and got an answer. Reading the superblock ourselves asks it quietly.
 *
 *   ext2/3/4   magic 0xEF53, 56 bytes into the superblock, which itself
 *              starts 1024 bytes in
 *   FAT        the boot sector ends in 0x55 0xAA and names itself, at
 *              0x36 for FAT12/16 and 0x52 for FAT32
 *
 * NULL when it is none of those, and then we fall back to trying. */
static const char *sniff_type(const char *dev)
{
    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return NULL;

    static u8 buf[2048];
    long n = lp_read((int)fd, buf, sizeof(buf));
    lp_close((int)fd);

    if (n < 2048)
        return NULL;

    /* ext: superblock at 1024, s_magic at +56 */
    u16 magic = (u16)(buf[1024 + 56] | (buf[1024 + 57] << 8));
    if (magic == 0xEF53)
        return "ext4";              /* ext2 and ext3 mount as ext4 too */

    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        if (memcmp(buf + 0x52, "FAT", 3) == 0 ||
            memcmp(buf + 0x36, "FAT", 3) == 0)
            return "vfat";
    }

    return NULL;
}

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

/* Split an -o string into the two things it actually holds.
 *
 * Some option words describe this mount and the kernel takes them as
 * bits - nosuid, ro. The rest describe the filesystem and are handed to
 * it as a string: size=64M for tmpfs, errors=remount-ro for ext4. They
 * have to be separated, because a filesystem's own parser rejects the
 * words it has never heard of, and "nosuid" is one of those.
 *
 * Returns the flags; `rest` gets everything not consumed. */
static unsigned long opt_flag(const char *word)
{
    if (strcmp(word, "bind")   == 0) return MS_BIND;
    if (strcmp(word, "ro")     == 0) return MS_RDONLY;
    if (strcmp(word, "noexec") == 0) return MS_NOEXEC;
    if (strcmp(word, "nosuid") == 0) return MS_NOSUID;
    if (strcmp(word, "nodev")  == 0) return MS_NODEV;
    return 0;
}

static unsigned long parse_options(const char *opts, char *rest, size_t size)
{
    unsigned long flags = 0;

    for (const char *p = opts; *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        char word[64];
        if (len >= sizeof(word))
            len = sizeof(word) - 1;
        memcpy(word, p, len);
        word[len] = '\0';

        unsigned long f = opt_flag(word);
        if (f) {
            flags |= f;
        } else if (word[0]) {
            if (rest[0])
                strlcat(rest, ",", size);
            strlcat(rest, word, size);
        }

        if (!comma)
            break;
        p = comma + 1;
    }
    return flags;
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
    char  data_buf[256];
    const char *data   = NULL;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    data_buf[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            flags |= parse_options(argv[++i], data_buf, sizeof(data_buf));
            data = data_buf[0] ? data_buf : NULL;
        } else if (nargs < 2) {
            args[nargs++] = argv[i];
        }
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO,
                "usage: mount [-t type] [-o options] <source> <dir>\n");
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
        long rc = lp_mount(src, dst, type, flags, data);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: %s -> %s (%s): failed (%ld)\n",
                    src, dst, type, -rc);
            return 1;
        }
        return 0;
    }

    /* A device that is not there is not an error worth a kernel log
     * entry. /etc/rc tries four names for the disk on purpose, knowing
     * three of them will not exist, and the boot screen should not fill
     * up with the kernel saying so. */
    if (strncmp(src, "/dev/", 5) == 0 && !lp_exists(src)) {
        dprintf(STDERR_FILENO, "mount: %s: no such device\n", src);
        return 1;
    }

    /* Ask the device what it holds before asking the kernel to guess. */
    const char *sniffed = sniff_type(src);
    if (sniffed) {
        long rc = lp_mount(src, dst, sniffed, flags, data);
        if (rc == 0) {
            printf("mount: %s -> %s (%s)\n", src, dst, sniffed);
            return 0;
        }
        /* It said one thing and would not mount as it: fall through and
         * try the rest, rather than trusting our own reading over the
         * filesystem driver's. */
    }

    /* Guess the type. */
    long last = -1;
    for (int i = 0; AUTO_TYPES[i]; i++) {
        long rc = lp_mount(src, dst, AUTO_TYPES[i], flags, data);
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
