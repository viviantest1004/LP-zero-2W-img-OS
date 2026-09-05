/* preinit - get off the RAM root and onto a real one.
 *
 * This is the /init of a very small initramfs, and it has exactly one
 * job: find the root filesystem, mount it, and hand over to the real
 * init on it. Nothing else belongs here. Everything this program does
 * happens before there is a system to report a failure to, so every
 * failure it can have says what it was and what to do about it, on the
 * console, before stopping.
 *
 * ── Why this exists ──
 *
 * The other images this project builds have no root filesystem at all:
 * the whole system is a cpio inside the kernel image, unpacked into RAM
 * at boot, and nothing on disk is part of the running system. That is
 * what makes a Raspberry Pi survive having its power pulled, and it is
 * the right answer for a board in a cupboard.
 *
 * It is the wrong answer for a machine somebody sits in front of. A
 * desktop wants /etc to still be there tomorrow, packages installed into
 * /usr rather than an overlay, and a root that can hold more than the
 * RAM it is unpacked into. So the amd64 image boots this instead: a
 * kernel with a small initramfs that mounts a real partition and
 * switches to it.
 *
 * ── How the handover works ──
 *
 * An initramfs cannot be unmounted - it is rootfs, the one mount the
 * kernel will not let go of. The way every Linux system does this is to
 * move the new root over the old one and chroot into it:
 *
 *     mount the real root at /newroot
 *     move /dev /proc /sys across, so they are not lost
 *     chdir /newroot
 *     mount --move . /
 *     chroot .
 *     exec /sbin/init
 *
 * The initramfs is then unreachable and its memory is reclaimed as the
 * files in it are freed. This program does not delete them first: it is
 * about 30KB and the /dev, /proc and /sys directories it made are
 * empty, so there is nothing worth the code to reclaim.
 *
 * ── Finding the root ──
 *
 * root= on the kernel command line, as a label or a device path. With
 * neither, the likely devices are tried in turn, which is what makes an
 * image written to an unknown machine boot at all: the disk may be sda,
 * vda, nvme0n1 or mmcblk0 and nothing before this point knows which.
 *
 * A label is preferred over a device name for the same reason `storage`
 * uses one: sda2 becomes sdb2 the moment another disk is plugged in,
 * and a root that moves is a machine that does not boot.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "disk.h"

#define NEWROOT   "/newroot"
#define DEF_LABEL "LPROOT"

/* Said on the console, where there is no logger yet. */
static void say(const char *msg)
{
    dprintf(STDERR_FILENO, "preinit: %s\n", msg);
}

static void sayf2(const char *a, const char *b)
{
    dprintf(STDERR_FILENO, "preinit: %s%s\n", a, b);
}

/* Nothing can be done, and there is no init to fall back to. Say why in
 * full, then stop somewhere a person can read it - panicking the kernel
 * would scroll the reason away. */
static void give_up(const char *why, const char *detail)
{
    dprintf(STDERR_FILENO, "\n");
    dprintf(STDERR_FILENO, "preinit: ** %s\n", why);
    if (detail && *detail)
        dprintf(STDERR_FILENO, "preinit:    %s\n", detail);
    dprintf(STDERR_FILENO,
        "preinit:\n"
        "preinit:    This kernel needs a root filesystem on disk. If the\n"
        "preinit:    disk is there, the label may be wrong: the root\n"
        "preinit:    partition has to be called %s, or root= on the\n"
        "preinit:    kernel command line has to name it.\n"
        "preinit:\n"
        "preinit:    cmdline.txt on the boot partition is editable from\n"
        "preinit:    any computer - it is FAT32.\n"
        "preinit:\n"
        "preinit:    Nothing has been written to any disk. It is safe to\n"
        "preinit:    switch the power off.\n", DEF_LABEL);
    for (;;)
        lp_sleep_ms(60000);
}

/* root=... from /proc/cmdline, or an empty string. */
static void root_from_cmdline(char *out, size_t n)
{
    out[0] = '\0';

    char buf[2048];
    if (proc_read("/proc/cmdline", buf, sizeof buf) <= 0)
        return;

    /* "root=" but not "rootwait=" or "rootflags=" - match at a word
     * boundary, or a machine with rootwait on the line would try to
     * mount a filesystem called "wait". */
    for (char *p = buf; *p; p++) {
        if (p != buf && p[-1] != ' ' && p[-1] != '\t')
            continue;
        if (strncmp(p, "root=", 5) != 0)
            continue;
        p += 5;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
               i < n - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        return;
    }
}

/* Try to mount `dev` as the root. Read-only first is deliberate: ext4
 * replays its journal at mount time either way, and a root mounted
 * read-write before anything has checked it is a root that a half
 * finished write can make worse. /etc/rc remounts it read-write once it
 * has looked. */
static bool try_mount(const char *dev, bool rw)
{
    unsigned long flags = rw ? 0UL : (unsigned long)MS_RDONLY;
    return lp_mount(dev, NEWROOT, "ext4", flags, NULL) == 0;
}

/* Does this device carry the label we are looking for? */
static bool has_label(const char *dev, const char *want)
{
    char fs[8] = "", label[24] = "";
    if (!disk_identify(dev, fs, sizeof fs, label, sizeof label))
        return false;
    return label[0] && strcmp(label, want) == 0;
}

/* Every partition on every disk the kernel found, in order. */
static int all_partitions(blk_t *out, int max)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);
    int n = 0;

    for (int i = 0; i < nd && n < max; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);
        for (int k = 0; k < np && n < max; k++) {
            if (parts[k].type == PART_TYPE_LINUX)
                out[n++] = parts[k];
        }
    }
    return n;
}

int main(void)
{
    /* The kernel gives an initramfs an empty /dev, so devtmpfs has to be
     * mounted before any disk can be opened by name. /proc is needed to
     * read the command line, /sys to enumerate the disks. */
    lp_mkdir("/proc", 0555);
    lp_mkdir("/sys",  0555);
    lp_mkdir("/dev",  0755);
    lp_mkdir(NEWROOT, 0755);

    lp_mount("proc",     "/proc", "proc",     0, NULL);
    lp_mount("sysfs",    "/sys",  "sysfs",    0, NULL);
    lp_mount("devtmpfs", "/dev",  "devtmpfs", 0, NULL);

    char want[128];
    root_from_cmdline(want, sizeof want);

    bool mounted = false;
    char chosen[64] = "";

    /* 1. root=/dev/something - take it at its word. */
    if (strncmp(want, "/dev/", 5) == 0) {
        sayf2("root= names ", want);
        for (int wait = 0; wait < 50 && !mounted; wait++) {
            if (try_mount(want, false)) {
                mounted = true;
                strlcpy(chosen, want, sizeof chosen);
            } else {
                /* USB and NVMe take a moment to appear. Five seconds is
                 * long enough for anything that is coming. */
                lp_sleep_ms(100);
            }
        }
        if (!mounted)
            give_up("the root named on the kernel command line is not there",
                    want);
    }

    /* 2. root=LABEL=x, or nothing at all, in which case the default. */
    const char *label = DEF_LABEL;
    if (!mounted && strncmp(want, "LABEL=", 6) == 0)
        label = want + 6;

    if (!mounted) {
        sayf2("looking for a root labelled ", label);

        /* The disks are not all there the instant the kernel starts.
         * Look repeatedly rather than once, and say so if it takes a
         * while - a silent five second pause looks like a hang. */
        for (int round = 0; round < 50 && !mounted; round++) {
            blk_t parts[DISK_MAX * DISK_PARTS];
            int np = all_partitions(parts, DISK_MAX * DISK_PARTS);

            for (int i = 0; i < np && !mounted; i++) {
                if (!has_label(parts[i].path, label))
                    continue;
                if (try_mount(parts[i].path, false)) {
                    mounted = true;
                    strlcpy(chosen, parts[i].path, sizeof chosen);
                }
            }
            if (!mounted) {
                if (round == 20)
                    say("still looking - a USB or NVMe disk can take a"
                        " few seconds to appear");
                lp_sleep_ms(100);
            }
        }
    }

    if (!mounted) {
        /* Say what IS there. "no root found" with nothing else is the
         * least useful message a computer can print. */
        blk_t parts[DISK_MAX * DISK_PARTS];
        int np = all_partitions(parts, DISK_MAX * DISK_PARTS);
        if (np == 0) {
            give_up("no disk with a Linux partition on it was found at all",
                    "the kernel may be missing the driver for this"
                    " machine's disk controller");
        }
        dprintf(STDERR_FILENO, "preinit: the partitions that are here:\n");
        for (int i = 0; i < np; i++) {
            char fs[8] = "", lb[24] = "";
            disk_identify(parts[i].path, fs, sizeof fs, lb, sizeof lb);
            dprintf(STDERR_FILENO, "preinit:   %-16s %-6s %s\n",
                    parts[i].path, fs[0] ? fs : "?",
                    lb[0] ? lb : "(no label)");
        }
        give_up("none of them is the root", "none carries the expected label");
    }

    sayf2("root is ", chosen);

    /* Take the kernel filesystems across. Mounting them again on the
     * other side would work too, but moving them keeps anything already
     * open on them - and it is one syscall instead of three plus the
     * unmounts. */
    lp_mkdir(NEWROOT "/proc", 0555);
    lp_mkdir(NEWROOT "/sys",  0555);
    lp_mkdir(NEWROOT "/dev",  0755);
    lp_mount("/proc", NEWROOT "/proc", NULL, MS_MOVE, NULL);
    lp_mount("/sys",  NEWROOT "/sys",  NULL, MS_MOVE, NULL);
    lp_mount("/dev",  NEWROOT "/dev",  NULL, MS_MOVE, NULL);

    /* The handover. After the chroot there is no way back, so anything
     * that could fail has already been done. */
    if (lp_chdir(NEWROOT) < 0)
        give_up("cannot enter the root that was just mounted", NEWROOT);
    if (lp_mount(".", "/", NULL, MS_MOVE, NULL) != 0)
        give_up("cannot move the new root over the old one",
                "the kernel refused MS_MOVE - is this really an initramfs?");
    if (lp_chroot(".") < 0)
        give_up("cannot chroot into the new root", NULL);
    if (lp_chdir("/") < 0)
        give_up("cannot enter / after the chroot", NULL);

    /* And hand over. init keeps pid 1, which is the whole point of
     * exec rather than fork. */
    static const char *INITS[] = {
        "/sbin/init", "/bin/init", "/init", "/usr/sbin/init", NULL
    };
    extern char **environ;
    for (int i = 0; INITS[i]; i++) {
        char *argv[] = { (char *)INITS[i], NULL };
        lp_execve(INITS[i], argv, environ);
    }

    give_up("the root filesystem has no init on it",
            "looked for /sbin/init, /bin/init, /init and /usr/sbin/init");
    return 1;                    /* not reached */
}
