/* update - replace the system, and be able to take it back.
 *
 *   update                      what is installed, and what it would go back to
 *   update <file>               install a system image from a file
 *   update <url>                fetch one and install it
 *   update --sha256 <hash> ...  and refuse it unless it hashes to this
 *   update --rollback           go back to the previous one now
 *   update --confirm            (boot) count the trial, roll back if it keeps failing
 *   update --commit             (5 minutes up) accept the trial
 *
 * ── Why the whole system is one file ──
 * The kernel and the entire userland are a single image: the root
 * filesystem is a cpio inside the kernel, unpacked into RAM at boot.
 * Nothing on any disk is part of the running system. That makes
 * updating it a file replacement rather than a package transaction, and
 * a file replacement is something you can do atomically.
 *
 * ── What is atomic and what is not ──
 * The new image is written to a second name, verified there, and only
 * then renamed over the one config.txt boots. A power cut before the
 * rename leaves the old system untouched; after it, the new one. The
 * rename is a single directory-entry write on FAT - not journalled, but
 * as close to atomic as this filesystem gets, and it is the only moment
 * of risk in the whole operation.
 *
 * ── What the rollback covers, and what it does not ──
 * Covered: the new system boots but is not healthy. It runs, so it can
 * count its own failures; three short boots and it puts the old image
 * back and reboots into it.
 *
 * Not covered: the new system does not execute at all. Nothing on this
 * board runs before the GPU firmware hands control to the kernel, and
 * that firmware cannot be told to try a second file. A Pi 4 or 5 has
 * tryboot for exactly this; the Zero 2 W does not.
 *
 * So the verification before the switch carries that weight, and it is
 * where the effort went: the hash must match if one was given, the file
 * must actually be an arm64 kernel, and the size must be plausible.
 * That covers the case this fails in practice, which is a download that
 * did not finish. For the rest, the previous image stays on the boot
 * partition and recovery is editing one line of config.txt with the
 * card in any PC - stated here rather than left to be discovered.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "disk.h"

#define BOOT      "/boot"
#define STANDBY   "/boot/system-standby.img"
#define STAGING   "/boot/system-new.img"
#define CONFIG    "/boot/config.txt"
#define EFI_BOOT  "/boot/EFI/BOOT/BOOTAA64.EFI"
#define TRIAL     "/data/.update-trial"

/* A system image is the kernel plus the whole userland. Below the first
 * figure it cannot be one; above the second something is wrong. */
#define MIN_IMAGE   (4UL  * 1048576)
#define MAX_IMAGE   (64UL * 1048576)

#define MAX_TRIES   3

/* ── /boot is read-only except when we are changing it ───────────── */

static bool boot_rw(void)
{
    return lp_mount(NULL, BOOT, NULL, MS_REMOUNT, NULL) == 0;
}

static void boot_ro(void)
{
    lp_sync();
    lp_mount(NULL, BOOT, NULL, MS_REMOUNT | MS_RDONLY, NULL);
}

/* ── which file does config.txt boot? ────────────────────────────── */

static bool active_image(char *out, size_t n)
{
    long fd = lp_open(CONFIG, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "kernel=", 7) != 0)
            continue;
        p += 7;
        char *end = p;
        while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
        *end = '\0';
        if (*p) {
            snprintf(out, n, "%s/%s", BOOT, p);
            found = true;
        }
        break;
    }
    lp_close((int)fd);
    return found;
}

/* ── is this actually a kernel? ──────────────────────────────────── */

/* An arm64 kernel built with CONFIG_EFI_STUB starts "MZ" and carries a
 * PE header, and every arm64 Image has "ARM\x64" at offset 0x38. Either
 * one is proof this is a kernel rather than half a download or an
 * HTML error page that got saved with the wrong name. */
static bool looks_like_a_kernel(const char *path, u64 *size_out)
{
    lp_stat_t st;
    if (lp_stat(path, &st, true) < 0)
        return false;
    if (size_out) *size_out = st.size;

    if (st.size < MIN_IMAGE || st.size > MAX_IMAGE) {
        dprintf(STDERR_FILENO,
                "update: %s is %lu MB. A system image is between %lu and %lu.\n",
                path, (unsigned long)(st.size / 1048576),
                MIN_IMAGE / 1048576, MAX_IMAGE / 1048576);
        return false;
    }

    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    u8 head[64];
    long n = lp_read((int)fd, head, sizeof head);
    lp_close((int)fd);
    if (n < (long)sizeof head)
        return false;

    bool mz  = head[0] == 'M' && head[1] == 'Z';
    bool arm = memcmp(head + 0x38, "ARM\x64", 4) == 0;

    if (!mz && !arm) {
        dprintf(STDERR_FILENO,
                "update: %s is not an arm64 kernel image.\n"
                "update:   No ARM64 magic and no PE header. This is what a\n"
                "update:   half-finished download looks like.\n", path);
        return false;
    }
    return true;
}

/* ── the trial record ────────────────────────────────────────────── */

static bool trial_read(char *prev, size_t n, int *tries)
{
    if (prev && n) prev[0] = '\0';
    if (tries) *tries = 0;

    long fd = lp_open(TRIAL, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    while (readline((int)fd, line, sizeof line) >= 0) {
        if (strncmp(line, "prev ", 5) == 0 && prev)
            strlcpy(prev, line + 5, n);
        else if (strncmp(line, "tries ", 6) == 0 && tries)
            *tries = atoi(line + 6);
    }
    lp_close((int)fd);
    return true;
}

static void trial_write(const char *prev, int tries)
{
    long fd = lp_open(TRIAL, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    dprintf((int)fd, "prev %s\ntries %d\n", prev, tries);
    lp_close((int)fd);
    lp_sync();
}

/* ── copying, with the space checked first ───────────────────────── */

static bool copy_file(const char *from, const char *to, u64 size)
{
    u64 free_b = 0, total_b = 0;
    lp_fs_space(BOOT, &free_b, &total_b);

    /* The existing destination is space we get back. */
    lp_stat_t old;
    u64 reclaim = (lp_stat(to, &old, true) == 0) ? old.size : 0;

    if (free_b + reclaim < size + 1048576) {
        char f[12], s[12];
        disk_human(free_b + reclaim, f, sizeof f);
        disk_human(size, s, sizeof s);
        dprintf(STDERR_FILENO,
                "update: the boot partition has %s free and this needs %s.\n"
                "update:   Nothing was changed.\n", f, s);
        return false;
    }

    long in = lp_open(from, O_RDONLY, 0);
    if (in < 0) {
        dprintf(STDERR_FILENO, "update: cannot read %s\n", from);
        return false;
    }
    long out = lp_open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO, "update: cannot write %s (%ld)\n", to, -out);
        lp_close((int)in);
        return false;
    }

    static char buf[65536];
    u64 done = 0;
    bool ok = true;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0) { ok = false; break; }
        if (lp_write((int)out, buf, (size_t)n) != n) {
            dprintf(STDERR_FILENO, "update: writing %s failed\n", to);
            ok = false;
            break;
        }
        done += (u64)n;
    }
    lp_close((int)in);
    lp_close((int)out);
    lp_sync();

    if (!ok || (size && done != size)) {
        lp_unlink(to);
        return false;
    }
    return true;
}

/* The same system, at the other name the same card boots from.
 *
 * This card boots two different ways. A real Pi's GPU firmware reads
 * config.txt and loads the file kernel= names. A virtual machine has no
 * GPU firmware; its UEFI looks for EFI/BOOT/BOOTAA64.EFI. Both are the
 * same system - an arm64 kernel with CONFIG_EFI_STUB is a valid PE
 * executable, so one file serves as both.
 *
 * Updating only one of them would leave a board that updates when you
 * put the card in a Pi and does nothing at all in a VM, with no
 * complaint either way. So both move together.
 *
 * mksdcard puts the compressed vmlinuz.efi there to save space on the
 * card; this replaces it with the uncompressed image, which UEFI boots
 * just as happily and is what we have in hand. */
static void mirror_to_efi(const char *image)
{
    if (!lp_exists(EFI_BOOT))
        return;                 /* this card has no UEFI path */

    lp_stat_t st;
    if (lp_stat(image, &st, true) < 0)
        return;

    if (copy_file(image, EFI_BOOT, st.size))
        printf("update: %s updated too (this card boots both ways)\n",
               EFI_BOOT);
    else
        dprintf(STDERR_FILENO,
                "update: ** %s could not be updated. A real Pi will run the\n"
                "update:    new system; a virtual machine will still run the\n"
                "update:    old one.\n", EFI_BOOT);
}

/* ── show ────────────────────────────────────────────────────────── */

static int show(void)
{
    char active[128];
    if (!active_image(active, sizeof active)) {
        dprintf(STDERR_FILENO,
                "update: %s does not say which kernel it boots.\n", CONFIG);
        return 1;
    }

    char hash[72] = "?";
    u64  size = 0;
    lp_stat_t st;
    if (lp_stat(active, &st, true) == 0)
        size = st.size;
    lp_sha256_file(active, hash);

    char sz[12];
    disk_human(size, sz, sizeof sz);
    printf("running   %s  %s\n", active, sz);
    printf("          %s\n", hash);

    if (lp_exists(STANDBY)) {
        char h2[72] = "?";
        u64 s2 = 0;
        if (lp_stat(STANDBY, &st, true) == 0) s2 = st.size;
        lp_sha256_file(STANDBY, h2);
        char sz2[12];
        disk_human(s2, sz2, sizeof sz2);
        printf("previous  %s  %s\n", STANDBY, sz2);
        printf("          %s\n", h2);
    } else {
        printf("previous  none - there is nothing to roll back to\n");
    }

    char prev[128];
    int tries = 0;
    if (trial_read(prev, sizeof prev, &tries)) {
        printf("\non trial  boot %d of %d\n", tries, MAX_TRIES);
        printf("          'update --commit' accepts it now,\n");
        printf("          'update --rollback' puts %s back.\n", prev);
        printf("          guard accepts it on its own after five minutes up.\n");
    }

    u64 free_b = 0, total_b = 0;
    lp_fs_space(BOOT, &free_b, &total_b);
    char f[12];
    disk_human(free_b, f, sizeof f);
    printf("\n%s has %s free\n", BOOT, f);
    printf("\n  update <file|url>     install a new system\n");
    printf("  update --rollback     go back to the previous one\n");
    return 0;
}

/* ── install ─────────────────────────────────────────────────────── */

static int install(const char *source, const char *want_hash)
{
    char active[128];
    if (!active_image(active, sizeof active)) {
        dprintf(STDERR_FILENO,
                "update: %s does not say which kernel it boots, so there is\n"
                "update:   nothing to replace.\n", CONFIG);
        return 1;
    }

    if (!boot_rw()) {
        dprintf(STDERR_FILENO,
                "update: cannot make %s writable. Is it mounted at all?\n",
                BOOT);
        return 1;
    }

    /* 1. Get it into place under a name nothing boots. */
    bool from_net = strncmp(source, "http://", 7) == 0 ||
                    strncmp(source, "https://", 8) == 0;
    if (from_net) {
        printf("update: fetching %s\n", source);
        if (net_http_get(source, STAGING) < 0) {
            lp_unlink(STAGING);
            boot_ro();
            return 1;
        }
    } else {
        if (!lp_exists(source)) {
            dprintf(STDERR_FILENO, "update: %s is not there\n", source);
            boot_ro();
            return 1;
        }
        lp_stat_t st;
        lp_stat(source, &st, true);
        printf("update: copying %s\n", source);
        if (!copy_file(source, STAGING, st.size)) {
            boot_ro();
            return 1;
        }
    }

    /* 2. Everything that can be checked, is checked - before anything
     *    the board boots from is touched. This is the part that has to
     *    be right, because nothing runs early enough to catch a kernel
     *    that will not start. */
    u64 size = 0;
    if (!looks_like_a_kernel(STAGING, &size)) {
        lp_unlink(STAGING);
        boot_ro();
        return 1;
    }

    char got[72];
    if (!lp_sha256_file(STAGING, got)) {
        dprintf(STDERR_FILENO, "update: cannot hash the new image\n");
        lp_unlink(STAGING);
        boot_ro();
        return 1;
    }

    if (want_hash) {
        if (strcmp(want_hash, got) != 0) {
            dprintf(STDERR_FILENO,
                    "update: ** the image is not the one you asked for.\n"
                    "update:    expected %s\n"
                    "update:    got      %s\n"
                    "update:    Nothing was changed.\n", want_hash, got);
            lp_unlink(STAGING);
            boot_ro();
            return 1;
        }
        printf("update: hash matches\n");
    } else {
        printf("update: %s\n", got);
        printf("update:   no --sha256 given, so this was not checked against\n");
        printf("update:   anything. It is an arm64 kernel of a plausible size\n");
        printf("update:   and that is all that is known about it.\n");
    }

    char sz[12];
    disk_human(size, sz, sizeof sz);
    printf("update: %s, verified\n", sz);

    /* 3. Keep the current one, so there is something to go back to. */
    lp_stat_t cur;
    if (lp_stat(active, &cur, true) == 0) {
        printf("update: keeping the current system as %s\n", STANDBY);
        if (!copy_file(active, STANDBY, cur.size)) {
            dprintf(STDERR_FILENO,
                    "update: could not keep a copy, so there would be no way\n"
                    "update:   back. Nothing was changed.\n");
            lp_unlink(STAGING);
            boot_ro();
            return 1;
        }
    }

    /* 4. The one moment that matters. Rename replaces the name the
     *    firmware reads, in a single directory-entry write. Before it,
     *    the old system boots; after it, the new one. */
    if (lp_rename(STAGING, active) < 0) {
        dprintf(STDERR_FILENO,
                "update: the rename failed, so the running system is still\n"
                "update:   in place and unchanged.\n");
        lp_unlink(STAGING);
        boot_ro();
        return 1;
    }
    mirror_to_efi(active);
    lp_sync();
    boot_ro();

    trial_write(STANDBY, 0);

    printf("\nupdate: installed. Reboot to run it.\n");
    printf("        It is on trial: three short boots and the old system\n");
    printf("        comes back by itself. Five minutes up and it is kept.\n");
    printf("\n        If it will not boot at all, nothing here can fix that -\n");
    printf("        put the card in a PC and change one line in config.txt:\n");
    const char *standby_name = strrchr(STANDBY, '/');
    printf("          kernel=%s\n",
           standby_name ? standby_name + 1 : STANDBY);
    return 0;
}

/* ── rollback ────────────────────────────────────────────────────── */

static int rollback(bool reboot_after)
{
    char active[128];
    if (!active_image(active, sizeof active))
        return 1;

    if (!lp_exists(STANDBY)) {
        dprintf(STDERR_FILENO,
                "update: there is no previous system to go back to.\n");
        return 1;
    }
    if (!looks_like_a_kernel(STANDBY, NULL)) {
        dprintf(STDERR_FILENO,
                "update: the previous system is not usable either. Not\n"
                "update:   touching the one that is at least running.\n");
        return 1;
    }

    if (!boot_rw()) {
        dprintf(STDERR_FILENO, "update: cannot make %s writable\n", BOOT);
        return 1;
    }

    /* The one that failed becomes the standby, so a rollback is itself
     * reversible and nothing is thrown away. */
    if (lp_rename(active, STAGING) < 0 ||
        lp_rename(STANDBY, active) < 0) {
        dprintf(STDERR_FILENO, "update: the rename failed\n");
        boot_ro();
        return 1;
    }
    lp_rename(STAGING, STANDBY);
    mirror_to_efi(active);
    lp_sync();
    boot_ro();

    lp_unlink(TRIAL);
    printf("update: the previous system is back in place.\n");

    if (reboot_after) {
        printf("update: rebooting.\n");
        lp_sleep_ms(1000);
        lp_sync();
        lp_reboot(LINUX_REBOOT_CMD_RESTART);
    }
    return 0;
}

/* ── what /etc/rc and guard call ─────────────────────────────────── */

static int confirm(void)
{
    char prev[128];
    int tries = 0;
    if (!trial_read(prev, sizeof prev, &tries))
        return 0;                       /* nothing on trial */

    tries++;
    if (tries >= MAX_TRIES) {
        dprintf(STDERR_FILENO,
                "update: this system has started %d times without staying up\n"
                "update:   for five minutes. Putting the previous one back.\n",
                tries);
        return rollback(true);
    }

    trial_write(prev, tries);
    printf("update: on trial, boot %d of %d\n", tries, MAX_TRIES);
    return 0;
}

static int commit(bool quiet)
{
    if (!lp_exists(TRIAL)) {
        if (!quiet)
            printf("update: nothing is on trial.\n");
        return 0;
    }
    lp_unlink(TRIAL);
    lp_sync();
    if (!quiet)
        printf("update: kept. %s is still there if it is needed.\n", STANDBY);
    return 0;
}

static void usage(void)
{
    printf("update - replace the system, and be able to take it back\n\n");
    printf("  update                    what is installed\n");
    printf("  update <file>             install from a file\n");
    printf("  update <url>              fetch one and install it\n");
    printf("  update --sha256 <hash> <source>\n");
    printf("  update --rollback         go back to the previous system\n");
    printf("  update --commit           accept the one on trial\n\n");
    printf("The whole system - kernel and userland - is one file, so an\n");
    printf("update is a file replacement. The new one is written under a\n");
    printf("different name, checked there, and only then renamed over the\n");
    printf("one config.txt boots.\n\n");
    printf("A system that boots but is not healthy rolls itself back after\n");
    printf("three short boots. A system that does not boot at all cannot -\n");
    printf("nothing on this board runs before the kernel does. That is why\n");
    printf("the checking happens before the switch, and why the previous\n");
    printf("image stays on the boot partition: recovery is then one line of\n");
    printf("config.txt, edited with the card in any PC.\n");
}

int main(int argc, char **argv)
{
    const char *want_hash = NULL;
    const char *source = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) {
            want_hash = argv[++i];
        } else if (strcmp(argv[i], "--rollback") == 0) {
            return rollback(false);
        } else if (strcmp(argv[i], "--confirm") == 0) {
            return confirm();
        } else if (strcmp(argv[i], "--commit") == 0) {
            return commit(false);
        } else if (strcmp(argv[i], "--commit-quiet") == 0) {
            return commit(true);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            source = argv[i];
        }
    }

    if (!source)
        return show();
    return install(source, want_hash);
}
