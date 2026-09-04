/* automount - notice drives being plugged in, and mount them.
 *
 *   automount              mount everything attached right now, once
 *   automount -d           the same, then keep watching (this is a daemon)
 *   automount -l           what is mounted, and what was skipped
 *   automount -u <name>    unmount one, by mount point or device
 *   automount -a           also mount drives that are not removable
 *
 * ── Why this exists ──
 * `disk` could already list every block device the kernel found, and
 * `mount` could mount one by hand. So plugging in a USB stick worked,
 * as long as you were logged in, knew it was called sdb1, and knew what
 * filesystem was on it. Nobody plugs a drive into a headless board and
 * then goes to look up its device name.
 *
 * ── How a drive announces itself ──
 * The kernel sends a message on a netlink socket every time a device
 * appears or goes away - the same messages udev listens to. They are
 * plain text: an "add@/devices/..." line followed by NUL-separated
 * KEY=VALUE pairs. We want four of them: ACTION, SUBSYSTEM, DEVNAME and
 * DEVTYPE. No library, no rules files, no daemon protocol.
 *
 * The alternative is polling /sys/block, which means a choice between
 * noticing late and burning CPU on a board that has neither to spare.
 *
 * ── What it will not touch ──
 * The disk this system booted from. Mounting a second copy of /boot or
 * /data under /media would be confusing at best, and at worst gives two
 * paths to the same bytes with different mount options - which is how
 * you get a corrupted filesystem rather than a full one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "disk.h"

#define MEDIA_ROOT      "/media"
#define MNT_ROOT        "/mnt"
#define STORAGE_CONF    "/data/storage.conf"
#define MAX_MOUNTS      16
#define UEVENT_BUF      4096

/* Netlink, the kernel's own object-event group. */
#define AF_NETLINK              16
#define SOCK_DGRAM_             2
#define NETLINK_KOBJECT_UEVENT  15
#define SOL_SOCKET_             1
#define SO_RCVBUF_              8

typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

static bool opt_all;            /* -a: 고정 디스크도 대상에 넣는다 */

/* ── which drives count as "plugged in" ──────────────────────────
 *
 * /sys/block/<dev>/removable is the obvious answer and it is wrong
 * often enough to matter. It reports the *media* as removable, which is
 * true of a card reader and false of most USB hard drives and of plenty
 * of USB sticks - QEMU's emulated USB disk reports 0 as well. Trusting
 * it alone means the drives people most want mounted are the ones that
 * get skipped, with no message.
 *
 * So we also look at how the device is attached. /sys/block/<dev> is a
 * symlink into the device tree, and for anything behind a USB host
 * controller that path contains "/usb". That is the question we
 * actually mean: did somebody just plug this in. */
static bool is_external(const blk_t *d)
{
    if (d->removable)
        return true;

    char link[512];
    char path[80];
    snprintf(path, sizeof path, "/sys/block/%s", d->name);

    long n = lp_readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return false;
    link[n] = '\0';

    return strstr(link, "/usb") != NULL;
}

/* ── what we must never touch ────────────────────────────────────── */

/* The whole disk that carries a given mount point, or empty. */
static void disk_behind(const char *mountpoint, char *out, size_t n)
{
    out[0] = '\0';

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char dev[64] = "", mnt[64] = "";
        int i = 0;
        while (line[i] && line[i] != ' ' && i < 63) { dev[i] = line[i]; i++; }
        dev[i] = '\0';
        if (line[i] == ' ') {
            i++;
            int j = 0;
            while (line[i] && line[i] != ' ' && j < 63) mnt[j++] = line[i++];
            mnt[j] = '\0';
        }

        if (strcmp(mnt, mountpoint) == 0 && dev[0] == '/') {
            if (!disk_whole(dev, out, n))
                strlcpy(out, dev, n);
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* The disk this system lives on. Everything on it is off limits. */
static bool is_system_disk(const char *disk_path)
{
    char boot_disk[40], data_disk[40];
    disk_behind("/boot", boot_disk, sizeof boot_disk);
    disk_behind("/data", data_disk, sizeof data_disk);

    if (boot_disk[0] && strcmp(disk_path, boot_disk) == 0)
        return true;
    if (data_disk[0] && strcmp(disk_path, data_disk) == 0)
        return true;
    return false;
}

/* ── choosing a name ─────────────────────────────────────────────── */

/* Turn a filesystem label into something safe to use as a directory.
 *
 * Labels come from whoever formatted the drive, which means they can
 * contain spaces, slashes and anything else. A label of "../../etc"
 * must not become a mount at /etc. Anything that is not a letter,
 * digit, dash, dot or underscore becomes a dash, and a leading dot is
 * dropped so nothing lands as a hidden directory. */
static void safe_name(const char *label, const char *fallback,
                      char *out, size_t n)
{
    size_t w = 0;
    if (label && label[0]) {
        for (size_t i = 0; label[i] && w + 1 < n; i++) {
            char c = label[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      (c == '.' && w > 0);
            out[w++] = ok ? c : '-';
        }
        /* Trailing dashes read as a typo. */
        while (w > 0 && out[w - 1] == '-') w--;
    }
    if (w == 0) {
        strlcpy(out, fallback, n);
        return;
    }
    out[w] = '\0';
}

/* /media/<name>, or /media/<name>-2 when that is taken by something
 * else. Two USB sticks both labelled "UNTITLED" is the normal case,
 * not a rare one. */
/* /media/<name>, or /media/<name>-2 when that is taken. Two USB sticks
 * both labelled "UNTITLED" is the normal case, not a rare one. */
static bool dir_exists(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    lp_close((int)fd);
    return true;
}

/* Is anything mounted here right now? An empty leftover directory is
 * fine to reuse; one with a filesystem on it is not. */
static bool is_mountpoint(const char *path)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return false;
    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        if (sp) {
            char *sp2 = strchr(sp + 1, ' ');
            if (sp2) *sp2 = '\0';
            if (strcmp(sp + 1, path) == 0)
                return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

static bool pick_mountpoint(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", MEDIA_ROOT, name);
    if (!dir_exists(out) || !is_mountpoint(out))
        return true;

    for (int suffix = 2; suffix < 20; suffix++) {
        snprintf(out, n, "%s/%s-%d", MEDIA_ROOT, name, suffix);
        if (!dir_exists(out) || !is_mountpoint(out))
            return true;
    }
    return false;
}

/* ── drives the system has been told to keep ─────────────────────
 *
 * `storage adopt` records a drive by filesystem label and gives it a
 * name. Such a drive belongs at /mnt/<name>, not under /media, because
 * things on the machine are configured to look for it there.
 *
 * Both programs read the same file rather than one telling the other,
 * so there is a single source of truth and no protocol between them to
 * fall out of step. */
static bool adopted_name_for(const char *label, char *out, size_t n)
{
    if (!label || !label[0])
        return false;

    long fd = lp_open(STORAGE_CONF, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char buf[2048];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *sp = strchr(line, ' ');
            if (sp) {
                *sp = '\0';
                if (strcmp(sp + 1, label) == 0) {
                    strlcpy(out, line, n);
                    return true;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

/* Labels this system uses for its own partitions. A drive carrying one
 * of these is not somebody's USB stick - it is a second copy of our own
 * card, and mounting it under /media would give two live paths to
 * filesystems that each expect to be the only one. */
static bool is_ours(const char *label)
{
    return label && label[0] &&
           (strcmp(label, "LPZERODATA") == 0 || strcmp(label, "LPZERO") == 0);
}

/* ── mounting ────────────────────────────────────────────────────── */

/* Filesystems worth trying, most likely first. A drive from a PC is
 * almost always one of these three. */
static const char *fs_candidates[] = { "ext4", "vfat", "exfat", "ext3", "ext2" };

static bool mount_one(const blk_t *p, bool quiet)
{
    char here[64];
    if (disk_mountpoint(p->path, here, sizeof here)) {
        if (!quiet)
            printf("  %-12s already mounted at %s\n", p->name, here);
        return false;
    }

    char whole[40];
    if (disk_whole(p->path, whole, sizeof whole) && is_system_disk(whole)) {
        if (!quiet)
            printf("  %-12s skipped - this is the system disk\n", p->name);
        return false;
    }

    if (is_ours(p->label)) {
        if (!quiet)
            printf("  %-12s skipped - one of our own partitions (%s)\n",
                   p->name, p->label);
        return false;
    }

    /* A drive the system has adopted goes to its fixed place. */
    char name[40], point[96], adopted[40];
    if (adopted_name_for(p->label, adopted, sizeof adopted)) {
        snprintf(point, sizeof point, "%s/%s", MNT_ROOT, adopted);
        lp_mkdir(MNT_ROOT, 0755);
    } else {
        safe_name(p->label, p->name, name, sizeof name);
        if (!pick_mountpoint(name, point, sizeof point)) {
            printf("  %-12s no free mount point under %s\n",
                   p->name, MEDIA_ROOT);
            return false;
        }
        lp_mkdir(MEDIA_ROOT, 0755);
    }

    if (lp_mkdir(point, 0755) < 0 && !dir_exists(point)) {
        printf("  %-12s cannot create %s\n", p->name, point);
        return false;
    }

    /* nosuid,nodev for the same reason /data has them: this filesystem
     * was written by another machine, and a setuid root binary or a
     * device node on it would be that machine's decision, not ours. */
    unsigned long flags = MS_NOSUID | MS_NODEV;

    /* Try what the superblock says first; fall back to the usual list,
     * because a filesystem we could not identify is not necessarily one
     * the kernel cannot mount. */
    if (p->fs[0] && lp_mount(p->path, point, p->fs, flags, NULL) == 0) {
        printf("  %-12s -> %s  (%s)\n", p->name, point, p->fs);
        return true;
    }

    for (u32 i = 0; i < sizeof fs_candidates / sizeof *fs_candidates; i++) {
        if (p->fs[0] && strcmp(p->fs, fs_candidates[i]) == 0)
            continue;                       /* already tried */
        if (lp_mount(p->path, point, fs_candidates[i], flags, NULL) == 0) {
            printf("  %-12s -> %s  (%s)\n", p->name, point, fs_candidates[i]);
            return true;
        }
    }

    /* Read-only is better than nothing: a dirty NTFS or an unclean
     * unmount often mounts read-only when it will not mount writable. */
    if (p->fs[0] &&
        lp_mount(p->path, point, p->fs, flags | MS_RDONLY, NULL) == 0) {
        printf("  %-12s -> %s  (%s, read-only)\n", p->name, point, p->fs);
        return true;
    }

    lp_rmdir(point);
    if (!quiet) {
        if (p->fs[0])
            printf("  %-12s %s is not a filesystem this kernel can mount\n",
                   p->name, p->fs);
        else
            printf("  %-12s no filesystem found on it\n", p->name);
    }
    return false;
}

static bool unmount_one(const char *point, bool quiet)
{
    long r = lp_umount(point, 0);
    if (r < 0) {
        /* Busy is the common case - somebody's shell is sitting in it.
         * Detach it from the tree so the drive can be pulled safely and
         * the last user's file handles die with them. */
        r = lp_umount(point, 2 /* MNT_DETACH */);
        if (r < 0) {
            if (!quiet)
                printf("  %s: cannot unmount (%ld)\n", point, -r);
            return false;
        }
        if (!quiet)
            printf("  %s: was busy, detached\n", point);
    }
    lp_rmdir(point);
    return true;
}

/* ── scanning what is here now ───────────────────────────────────── */

static int scan_and_mount(bool quiet)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);
    int mounted = 0;

    for (int i = 0; i < nd; i++) {
        if (is_system_disk(disks[i].path))
            continue;
        if (!is_external(&disks[i]) && !opt_all)
            continue;

        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);

        if (np <= 0) {
            /* No partition table. A drive formatted whole is common on
             * USB sticks, so try the disk itself. */
            if (mount_one(&disks[i], quiet))
                mounted++;
            continue;
        }
        for (int j = 0; j < np; j++)
            if (mount_one(&parts[j], quiet))
                mounted++;
    }
    return mounted;
}

/* A device went away. Unmount anything under /media that no longer has
 * a device behind it.
 *
 * The device is already gone by the time we hear about it, so we cannot
 * ask what it was - we can only look at what is mounted and check which
 * of those devices still exist. */
static int drop_vanished(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return 0;
    buf[got] = '\0';

    int dropped = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char dev[64] = "", mnt[80] = "";
        int i = 0, j = 0;
        while (line[i] && line[i] != ' ' && j < 63) dev[j++] = line[i++];
        dev[j] = '\0';
        if (line[i] == ' ') {
            i++; j = 0;
            while (line[i] && line[i] != ' ' && j < 79) mnt[j++] = line[i++];
            mnt[j] = '\0';
        }

        if (strncmp(mnt, MEDIA_ROOT "/", sizeof MEDIA_ROOT) == 0 &&
            dev[0] == '/' && lp_access(dev, F_OK) != 0) {
            printf("automount: %s went away, releasing %s\n", dev, mnt);
            if (unmount_one(mnt, true))
                dropped++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return dropped;
}

/* ── listening ───────────────────────────────────────────────────── */

/* One uevent. Returns true when it is a block device appearing or
 * disappearing - which is the only kind we act on. */
static bool parse_uevent(const char *msg, size_t len,
                         char *action, size_t an,
                         char *devname, size_t dn)
{
    action[0] = '\0';
    devname[0] = '\0';
    bool is_block = false;

    /* Fields are NUL-separated, after a first line we do not need. */
    for (size_t i = 0; i < len; ) {
        const char *field = msg + i;
        size_t flen = strlen(field);

        if (strncmp(field, "ACTION=", 7) == 0)
            strlcpy(action, field + 7, an);
        else if (strncmp(field, "DEVNAME=", 8) == 0)
            strlcpy(devname, field + 8, dn);
        else if (strncmp(field, "SUBSYSTEM=", 10) == 0)
            is_block = strcmp(field + 10, "block") == 0;

        i += flen + 1;
        if (flen == 0)
            break;
    }
    return is_block && action[0] && devname[0];
}

static int watch(void)
{
    long fd = lp_socket(AF_NETLINK, SOCK_DGRAM_, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "automount: cannot open the kernel's event socket (%ld)\n", -fd);
        return 1;
    }

    /* A burst of events arrives when a drive with several partitions is
     * plugged in, and the default receive buffer is small enough to drop
     * some of them. A dropped event means a partition that never gets
     * mounted and no sign of why. */
    int rcvbuf = 1 << 18;
    lp_setsockopt((int)fd, SOL_SOCKET_, SO_RCVBUF_, &rcvbuf, sizeof rcvbuf);

    sockaddr_nl_t sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = 0;               /* let the kernel assign */
    sa.nl_groups = 1;               /* the uevent broadcast group */

    if (lp_bind((int)fd, &sa, sizeof sa) < 0) {
        dprintf(STDERR_FILENO,
                "automount: cannot listen for device events\n");
        lp_close((int)fd);
        return 1;
    }

    printf("automount: watching for drives\n");

    char buf[UEVENT_BUF];
    for (;;) {
        long got = lp_recvfrom((int)fd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (got <= 0)
            continue;
        buf[got] = '\0';

        char action[16], devname[48];
        if (!parse_uevent(buf, (size_t)got, action, sizeof action,
                          devname, sizeof devname))
            continue;

        if (strcmp(action, "add") == 0) {
            /* The device node may not exist for a moment after the
             * event - devtmpfs creates it around the same time. Give it
             * a beat rather than reporting a drive that is really there
             * as missing. */
            char path[64];
            snprintf(path, sizeof path, "/dev/%s", devname);
            for (int t = 0; t < 20 && lp_access(path, F_OK) != 0; t++)
                lp_sleep_ms(50);

            printf("automount: %s appeared\n", devname);
            scan_and_mount(true);
        } else if (strcmp(action, "remove") == 0) {
            drop_vanished();
        }
    }
}

/* ── listing ─────────────────────────────────────────────────────── */

static void list_mounts(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        printf("cannot read /proc/mounts\n");
        return;
    }
    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return;
    buf[got] = '\0';

    int n = 0;
    printf("mounted under %s:\n", MEDIA_ROOT);
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "/dev/", 5) == 0 && strstr(line, MEDIA_ROOT "/")) {
            printf("  %s\n", line);
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (n == 0)
        printf("  (nothing)\n");
}

/* ── entry ───────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: automount [-d] [-a] [-l] [-u <name>]\n");
    printf("  (no options)  mount every drive attached right now\n");
    printf("  -d            keep watching for drives being plugged in\n");
    printf("  -a            include internal drives too, not just plugged-in ones\n");
    printf("  -l            list what is mounted under %s\n", MEDIA_ROOT);
    printf("  -u <name>     unmount one, by mount point or device\n");
}

int main(int argc, char **argv)
{
    bool daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_all = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            list_mounts();
            return 0;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            const char *what = argv[++i];
            char point[96];
            if (what[0] == '/')
                strlcpy(point, what, sizeof point);
            else
                snprintf(point, sizeof point, "%s/%s", MEDIA_ROOT, what);
            return unmount_one(point, false) ? 0 : 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            dprintf(STDERR_FILENO, "automount: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    lp_mkdir(MEDIA_ROOT, 0755);

    printf("automount: looking at what is attached\n");
    int n = scan_and_mount(false);
    if (n == 0)
        printf("  nothing new to mount\n");

    if (daemon)
        return watch();
    return 0;
}
