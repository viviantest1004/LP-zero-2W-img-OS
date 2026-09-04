/* persist - keep what gets installed into the root filesystem
 *
 * The root filesystem of this machine is a cpio archive inside the
 * kernel image, unpacked into RAM at every boot. That is what makes the
 * system recover from anything: whatever a bad day did to /bin, the next
 * boot has the original back. It also means that anything installed
 * there is gone at the next power cut, which is the wrong answer to
 * `cp mytool /bin/` - the command succeeds, the file is there, and a
 * reboot silently undoes it.
 *
 * `pkg` sidesteps this by installing to /data/bin, and python packages
 * go to /data/python. But a program that was built somewhere else, or a
 * `make install` that writes to /usr/local, or a file copied into /bin
 * by hand, all landed in RAM.
 *
 * So each of those directories becomes an overlay: the copy in the
 * kernel image underneath, a directory on /data on top. Reads see both,
 * writes land on /data, and the next boot puts the overlay back. Nothing
 * has to know about it - `cp`, `make install`, a tarball unpacked with
 * tar, they all just work and the result is still there tomorrow.
 *
 * What this deliberately does NOT cover:
 *
 *   /etc  - the boot reads /etc/services and /etc/rc before /data is
 *           even mounted, and a stale copy of either shadowing a
 *           system update is how a board stops booting. Configuration
 *           that is meant to last already has its own files on /data.
 *   /tmp  - it is supposed to be lost. That is what it is for.
 *   /var  - runtime state. Writing service pids to the card every time
 *           a service restarts is wear for nothing.
 *
 * The overlays are mounted nosuid and nodev, for the same reason /data
 * itself is: the upper directories are on a card that can be taken to
 * another machine, and without those flags a setuid-root file written
 * there from a PC would be setuid-root here. Nothing in this system is
 * setuid, so nothing legitimate is lost.
 *
 * One consequence worth knowing. If a system update ships a new
 * /bin/foo and an older /bin/foo is in the upper layer, the old one
 * wins - that is what "on top" means. `persist` on its own lists what
 * is up there, and `persist forget <name>` removes one file so the
 * image's copy shows through again.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

/* getdents64 records: reclen at byte 16, the name at 19. */
#define DIRENT_RECLEN  16
#define DIRENT_NAME    19

#define UPPER_ROOT  "/data/persist"
#define WORK_ROOT   "/data/persist/.work"

/* The directories a person or an installer writes programs into. */
static const char *DIRS[] = {
    "/bin", "/sbin", "/lib", "/usr", "/opt", "/srv", NULL
};

static bool ensure_dir(const char *path)
{
    if (lp_is_dir(path))
        return true;
    if (lp_mkdir(path, 0755) == 0)
        return true;
    return lp_is_dir(path);
}

/* /bin -> bin, for building the paths under /data. */
static const char *leaf(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool already_overlaid(const char *dir)
{
    char buf[8192];
    if (proc_read("/proc/mounts", buf, sizeof buf) <= 0)
        return false;

    for (const char *p = buf; *p; ) {
        /* "overlay /bin overlay rw,... 0 0" */
        if (strncmp(p, "overlay ", 8) == 0) {
            const char *m = p + 8;
            size_t n = strlen(dir);
            if (strncmp(m, dir, n) == 0 && m[n] == ' ')
                return true;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return false;
}

static bool mount_one(const char *dir, bool loud)
{
    char upper[128], work[128], opts[512];

    if (!ensure_dir(dir)) {
        if (loud)
            dprintf(STDERR_FILENO,
                    "persist: %s is not there and cannot be created\n", dir);
        return false;
    }

    snprintf(upper, sizeof upper, "%s/%s", UPPER_ROOT, leaf(dir));
    snprintf(work,  sizeof work,  "%s/%s", WORK_ROOT,  leaf(dir));

    if (!ensure_dir(upper) || !ensure_dir(work)) {
        if (loud)
            dprintf(STDERR_FILENO,
                    "persist: cannot make %s on the data partition -"
                    " is it mounted and writable?\n", upper);
        return false;
    }

    /* index=off and xino=off, and they are not optional here.
     *
     * Both features record the identity of a file in the lower layer -
     * an inode number, a file handle - and check it on the next mount.
     * That is right when the lower layer is a filesystem on a disk. Ours
     * is a cpio archive unpacked into RAM at every boot, so every inode
     * number is new every time, verification fails, and the mount is
     * refused with ESTALE. Which is exactly what happened: the first
     * boot mounted cleanly and the second refused all six, so the files
     * installed on the first boot were invisible - the failure this
     * program exists to prevent, arriving one boot later.
     *
     * redirect_dir stays on: it records a path, and paths do not change
     * between boots. */
    snprintf(opts, sizeof opts,
             "lowerdir=%s,upperdir=%s,workdir=%s,index=off,xino=off",
             dir, upper, work);

    long rc = lp_mount("overlay", dir, "overlay",
                       MS_NOSUID | MS_NODEV, opts);
    if (rc < 0) {
        if (loud)
            dprintf(STDERR_FILENO,
                    "persist: cannot keep %s (%ld) - files written there"
                    " will be lost at the next boot\n", dir, -rc);
        return false;
    }
    return true;
}

/* Count the files a directory tree holds, and their bytes. */
static void measure(const char *path, int *files, long *bytes, int depth)
{
    if (depth > 8)
        return;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;

        for (long off = 0; off < got; ) {
            char *rec  = buf + off;
            u16   rlen = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (rlen == 0)
                break;
            off += rlen;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", path, name);

            lp_stat_t st;
            if (lp_stat(sub, &st, false) < 0)
                continue;

            if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
                measure(sub, files, bytes, depth + 1);
            } else {
                (*files)++;
                *bytes += (long)st.size;
            }
        }
    }
    lp_close((int)fd);
}

static void show(void)
{
    printf("Files installed into the system are kept on the data"
           " partition,\nso they are still there after a reboot.\n\n");

    int  total_files = 0;
    long total_bytes = 0;

    for (int i = 0; DIRS[i]; i++) {
        char upper[128];
        snprintf(upper, sizeof upper, "%s/%s", UPPER_ROOT, leaf(DIRS[i]));

        int  files = 0;
        long bytes = 0;
        measure(upper, &files, &bytes, 0);
        total_files += files;
        total_bytes += bytes;

        printf("  %-8s %-6s %d file%s", DIRS[i],
               already_overlaid(DIRS[i]) ? "kept" : "**",
               files, files == 1 ? "" : "s");
        if (bytes >= 1024 * 1024)
            printf("  %ld MB", bytes / (1024 * 1024));
        else if (bytes > 0)
            printf("  %ld KB", (bytes + 1023) / 1024);
        printf("\n");
    }

    printf("\n  %d file%s in all, %ld KB on the card.\n",
           total_files, total_files == 1 ? "" : "s",
           (total_bytes + 1023) / 1024);

    for (int i = 0; DIRS[i]; i++) {
        if (!already_overlaid(DIRS[i])) {
            printf("\n  ** is a directory that is NOT being kept: anything"
                   " written there\n"
                   "     is in RAM and goes at the next boot. That happens"
                   " when the data\n"
                   "     partition is missing or read-only - `disk` and"
                   " `fsck` say which.\n");
            break;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "on") == 0) {
        /* Called from /etc/rc, once, after /data is mounted. Quiet when
         * it works: the boot has enough to say already. */
        if (!lp_is_dir("/data")) {
            dprintf(STDERR_FILENO,
                    "persist: no /data - nothing installed into the system"
                    " will survive a reboot\n");
            return 1;
        }
        if (!ensure_dir(UPPER_ROOT) || !ensure_dir(WORK_ROOT)) {
            dprintf(STDERR_FILENO,
                    "persist: cannot write to %s - nothing installed into"
                    " the system will survive a reboot\n", UPPER_ROOT);
            return 1;
        }

        int kept = 0, failed = 0;
        for (int i = 0; DIRS[i]; i++) {
            if (already_overlaid(DIRS[i])) { kept++; continue; }
            if (mount_one(DIRS[i], true)) kept++;
            else                          failed++;
        }
        if (failed)
            dprintf(STDERR_FILENO,
                    "persist: keeping %d of %d directories\n",
                    kept, kept + failed);
        return failed ? 1 : 0;
    }

    if (argc > 2 && strcmp(argv[1], "forget") == 0) {
        /* Remove one file from the upper layer, so the copy in the
         * kernel image shows through again. */
        const char *want = argv[2];
        int removed = 0;

        for (int i = 0; DIRS[i]; i++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s/%s",
                     UPPER_ROOT, leaf(DIRS[i]), leaf(want));
            lp_stat_t st;
            if (lp_stat(p, &st, false) < 0)
                continue;
            if (lp_unlink(p) == 0) {
                printf("persist: forgot %s/%s\n", DIRS[i], leaf(want));
                removed++;
            } else {
                dprintf(STDERR_FILENO,
                        "persist: cannot remove %s\n", p);
            }
        }

        if (!removed) {
            dprintf(STDERR_FILENO,
                    "persist: nothing called %s was added to the system\n",
                    leaf(want));
            return 1;
        }
        printf("persist:   the version in the system image is back."
               " It takes a reboot\n"
               "persist:   to see it, because the old one is still open.\n");
        return 0;
    }

    if (argc > 1) {
        printf("usage: persist            what is being kept\n");
        printf("       persist on         start keeping it (from /etc/rc)\n");
        printf("       persist forget <name>\n");
        printf("                          drop one added file, so the\n");
        printf("                          system's own copy is used again\n");
        return argc > 1 && strcmp(argv[1], "-h") == 0 ? 0 : 2;
    }

    show();
    return 0;
}
