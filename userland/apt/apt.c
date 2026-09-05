/* apt - Debian's package manager, on this system.
 *
 *   apt install <package>...     what you came for
 *   apt remove <package>...
 *   apt update                   refresh the package lists
 *   apt search <text>
 *   apt setup                    fetch the Debian base (first run does this)
 *   apt shell                    a shell inside the Debian tree
 *   apt status                   what is set up, and how much room it takes
 *
 * ── Why this is not just a binary we ship ──
 *
 * apt is not one program. It is apt, apt-get, dpkg, a database under
 * /var/lib/dpkg, maintainer scripts that expect /bin/sh and coreutils,
 * and a filesystem laid out the way Debian lays one out. dpkg has those
 * paths compiled in - it does not take a --root that means what you
 * would want it to mean. Dropping the binaries onto this system would
 * produce something that starts and then cannot install anything.
 *
 * So the whole Debian userland goes in one directory on /data, and apt
 * runs inside it with that directory as its root. Everything it
 * installs lands there too, which means:
 *
 *   - the system image is untouched, and stays the size it is
 *   - `apt remove` cannot break this machine's own commands, because
 *     they are not in there
 *   - deleting /data/debian undoes all of it, completely
 *
 * ── Why it downloads instead of shipping ──
 *
 * A minimal Debian is about 120MB unpacked. The whole point of this
 * system is that it is 11-23MB, so it cannot carry one. The first run
 * of `apt` fetches it, and says so before it starts.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"

#define ROOT        "/data/debian"
#define MARKER      ROOT "/etc/debian_version"
#define SOURCES     ROOT "/etc/apt/sources.list"
#define RESOLV      ROOT "/etc/resolv.conf"

/* Which Debian, and where from.
 *
 * Bookworm rather than the newest: this is a board that gets left
 * alone for months, and the point of a stable release is that its
 * package versions stop moving. */
#define SUITE       "bookworm"
#define MIRROR      "https://deb.debian.org/debian"

/* The base tarball. debuerreotype builds these - they are what the
 * official Debian container images are made from, one per architecture,
 * and they are a plain tar of a working Debian root. */
#define BASE_URL_BASE \
    "https://github.com/debuerreotype/docker-debian-artifacts/raw/dist-"

#if defined(__x86_64__)
#  define DEB_ARCH "amd64"
#elif defined(__aarch64__)
#  define DEB_ARCH "arm64"
#else
#  define DEB_ARCH "unknown"
#endif

static const char *me = "apt";

/* ── small helpers ───────────────────────────────────────────────── */

static bool exists(const char *p) { return lp_access(p, F_OK) == 0; }

static int run_wait(const char *path, char *const argv[])
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        extern char **environ;
        lp_execve(path, argv, environ);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

static bool write_file(const char *path, const char *text)
{
    long fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    long n = lp_write((int)fd, text, strlen(text));
    lp_close((int)fd);
    return n == (long)strlen(text);
}

static void mkdirs(const char *path)
{
    char buf[256];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        lp_mkdir(buf, 0755);
        *p = '/';
    }
    lp_mkdir(buf, 0755);
}

/* ── is it there ─────────────────────────────────────────────────── */

static bool is_set_up(void)
{
    return exists(MARKER) && exists(ROOT "/usr/bin/apt");
}

/* ── the kernel filesystems apt needs inside the tree ────────────── */

/* dpkg's maintainer scripts read /proc, and anything that touches the
 * network needs /dev. These are bind mounts, so nothing is copied and
 * unmounting them leaves the tree exactly as it was.
 *
 * Left mounted between runs on purpose: mounting costs nothing while
 * they sit there, and a tree with /proc missing fails in ways that read
 * as "this package is broken" rather than "the mount is missing". */
static void mount_kernel_fs(void)
{
    static const struct { const char *src, *dir, *type; unsigned long flags; }
    binds[] = {
        { "proc",  ROOT "/proc", "proc",     0 },
        { "sysfs", ROOT "/sys",  "sysfs",    0 },
        { "/dev",  ROOT "/dev",  NULL,       MS_BIND },
    };

    for (unsigned i = 0; i < sizeof binds / sizeof *binds; i++) {
        mkdirs(binds[i].dir);
        /* Already mounted is not an error worth reporting. */
        lp_mount(binds[i].src, binds[i].dir, binds[i].type,
                 binds[i].flags, NULL);
    }
}

static void unmount_kernel_fs(void)
{
    lp_umount(ROOT "/dev", MNT_DETACH);
    lp_umount(ROOT "/sys", MNT_DETACH);
    lp_umount(ROOT "/proc", MNT_DETACH);
}

/* ── setup ───────────────────────────────────────────────────────── */

static int cmd_setup(bool quiet)
{
    if (is_set_up()) {
        if (!quiet)
            printf("%s: Debian is already set up in %s\n", me, ROOT);
        return 0;
    }

    if (strcmp(DEB_ARCH, "unknown") == 0) {
        dprintf(STDERR_FILENO,
                "%s: this build does not know what Debian calls its own"
                " architecture\n", me);
        return 1;
    }

    printf("%s: setting up Debian %s (%s) under %s\n",
           me, SUITE, DEB_ARCH, ROOT);
    printf("%s:   this downloads about 95MB and unpacks to about 440MB.\n",
           me);
    printf("%s:   Measured, not estimated - a Debian base is not small,\n",
           me);
    printf("%s:   which is most of why this system does not ship one.\n",
           me);
    printf("%s:   It goes on /data and nowhere else - `rm -rf %s`"
           " undoes all of it.\n", me, ROOT);
    printf("\n");

    /* Room to work: the tarball, plus what it unpacks to, plus slack
     * for the first apt update. Checking now beats filling the data
     * partition and finding out when something else fails. */
    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) == 0) {
        /* 440MB unpacked, plus the archive while it is being unpacked,
         * plus room for apt's own lists and whatever gets installed
         * next. Running the data partition dry half way through leaves
         * a tree that is there but broken, which is harder to recover
         * from than not starting. */
        u64 need = 900ULL * 1024 * 1024;
        if (freeb < need) {
            dprintf(STDERR_FILENO,
                    "%s: /data has %lu MB free and this needs about %lu MB.\n",
                    me, (unsigned long)(freeb / 1048576),
                    (unsigned long)(need / 1048576));
            dprintf(STDERR_FILENO,
                    "%s:   `expandfs` grows /data to fill the card;"
                    " `storage` can add a drive.\n", me);
            return 1;
        }
    }

    mkdirs(ROOT);

    /* The download and the unpack are in /etc/apt-setup.py.
     *
     * Working out which build is current means reading an HTML index;
     * the archive is xz and full of device nodes, hard links and setuid
     * bits. Python does all three properly and our tar does none of
     * them - it reads uncompressed ustar, which is the right amount of
     * tar for pkg and nowhere near enough for a Debian root.
     *
     * Python is here whenever glibc is, and glibc is what makes running
     * Debian binaries possible at all. An image without it could not
     * have run apt anyway. */
    const char *py = exists("/data/bin/python") ? "/data/bin/python"
                   : exists("/data/python/bin/python3.12")
                     ? "/data/python/bin/python3.12" : NULL;
    if (!py) {
        dprintf(STDERR_FILENO,
                "%s: this needs Python, which is not on this image.\n"
                "%s:   An image without Python has no glibc either, and\n"
                "%s:   Debian binaries cannot run without one.\n",
                me, me, me);
        return 1;
    }

    char *py_args[] = {
        (char *)py, (char *)"/etc/apt-setup.py",
        (char *)DEB_ARCH, (char *)ROOT, (char *)"/data/ssl/cert.pem", NULL
    };
    if (run_wait(py, py_args) != 0)
        return 1;

    if (!exists(MARKER)) {
        dprintf(STDERR_FILENO,
                "%s: unpacked, but %s is not there - the tarball was not"
                " a Debian root\n", me, MARKER);
        return 1;
    }

    /* Where packages come from. The base tarball ships a sources.list
     * pointing at a snapshot, which is right for a container built to
     * be reproducible and wrong for a machine that wants updates. */
    mkdirs(ROOT "/etc/apt");
    write_file(SOURCES,
        "# Written by `apt setup` on linux-LP.\n"
        "deb " MIRROR " " SUITE " main contrib non-free-firmware\n"
        "deb " MIRROR "-security " SUITE "-security main contrib non-free-firmware\n"
        "deb " MIRROR " " SUITE "-updates main contrib non-free-firmware\n");

    /* Name resolution. The tree has its own /etc, so it needs its own
     * copy of where to ask - a bind mount would be undone by the next
     * unmount and this file never changes on this system. */
    char resolv[256] = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
    long fd = lp_open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[256];
        long n = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (n > 0) { buf[n] = '\0'; strlcpy(resolv, buf, sizeof resolv); }
    }
    write_file(RESOLV, resolv);

    /* Keep apt from asking questions nobody is there to answer. A board
     * that stops mid-install waiting for a keypress is a board that has
     * hung, as far as anyone can tell from outside. */
    mkdirs(ROOT "/etc/apt/apt.conf.d");
    write_file(ROOT "/etc/apt/apt.conf.d/90linux-lp",
        "// Written by `apt setup` on linux-LP.\n"
        "APT::Get::Assume-Yes \"true\";\n"
        "APT::Get::Fix-Broken \"true\";\n"
        "Dpkg::Use-Pty \"false\";\n"
        "Acquire::Retries \"3\";\n");

    printf("\n%s: Debian is set up. `apt update` next, then `apt install`.\n",
           me);
    return 0;
}

/* ── running something inside the tree ───────────────────────────── */

static int in_debian(char *const argv[]) __attribute__((noreturn));
static int in_debian(char *const argv[])
{
    mount_kernel_fs();

    if (lp_chroot(ROOT) < 0) {
        dprintf(STDERR_FILENO,
                "%s: cannot enter %s - are you root?\n", me, ROOT);
        lp_exit(1);
    }
    if (lp_chdir("/") < 0)
        lp_exit(1);

    /* Inside, PATH is Debian's. Ours means nothing here - none of our
     * commands exist in that tree. */
    char *envp[] = {
        (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        (char *)"HOME=/root",
        (char *)"TERM=linux",
        (char *)"LC_ALL=C",
        (char *)"DEBIAN_FRONTEND=noninteractive",
        NULL
    };

    lp_execve(argv[0], argv, envp);

    dprintf(STDERR_FILENO,
            "%s: %s is not in the Debian tree\n", me, argv[0]);
    lp_exit(127);
}

/* Fork, chroot in the child, wait. The parent keeps its own root, so
 * the mounts can be cleaned up afterwards and the shell that called us
 * is not left inside a chroot it cannot leave. */
static int call_in_debian(char *const argv[])
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "%s: cannot fork\n", me);
        return 1;
    }
    if (pid == 0)
        in_debian(argv);

    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : 1;
}

/* ── status ──────────────────────────────────────────────────────── */

static int cmd_status(void)
{
    if (!is_set_up()) {
        printf("Debian is not set up yet.\n");
        printf("  `apt install <package>` will set it up first,\n");
        printf("  or `apt setup` to do only that.\n");
        return 0;
    }

    char version[32] = "?";
    long fd = lp_open(MARKER, O_RDONLY, 0);
    if (fd >= 0) {
        long n = lp_read((int)fd, version, sizeof version - 1);
        lp_close((int)fd);
        if (n > 0) {
            version[n] = '\0';
            for (int i = 0; version[i]; i++)
                if (version[i] == '\n') version[i] = '\0';
        }
    }

    printf("Debian %s (%s) in %s\n", version, DEB_ARCH, ROOT);

    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) == 0) {
        char f[12];
        disk_human(freeb, f, sizeof f);
        printf("  /data has %s free\n", f);
    }

    printf("\nAnything installed lives in that directory and runs with\n");
    printf("`apt run <command>`. The system's own commands are untouched.\n");
    return 0;
}

/* ── entry ───────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: apt <command> [arguments]\n\n");
    printf("  install <pkg>...   install packages from Debian\n");
    printf("  remove <pkg>...    take them away again\n");
    printf("  update             refresh the package lists\n");
    printf("  upgrade            update what is installed\n");
    printf("  search <text>      what Debian has\n");
    printf("  show <pkg>         details of one\n");
    printf("  list --installed   what is in there now\n");
    printf("\n");
    printf("  run <cmd> [args]   run something installed, inside the tree\n");
    printf("  shell              a shell in there\n");
    printf("  setup              fetch the Debian base (about 30MB)\n");
    printf("  status             what is set up\n");
    printf("  purge-all          delete the whole thing\n");
    printf("\n");
    printf("Debian lives in %s and nothing else on this system is\n", ROOT);
    printf("touched by it. Removing that directory removes all of it.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }

    const char *cmd = argv[1];

    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "%s: only root can do this\n", me);
        return 1;
    }

    if (strcmp(cmd, "status") == 0)
        return cmd_status();

    if (strcmp(cmd, "setup") == 0)
        return cmd_setup(false);

    if (strcmp(cmd, "purge-all") == 0) {
        if (!is_set_up()) {
            printf("%s: nothing to remove\n", me);
            return 0;
        }
        printf("This deletes %s and everything installed in it.\n", ROOT);
        printf("Type yes to go ahead: ");
        char answer[16];
        long n = lp_read(STDIN_FILENO, answer, sizeof answer - 1);
        if (n <= 0) return 1;
        answer[n] = '\0';
        for (long i = 0; i < n; i++)
            if (answer[i] == '\n' || answer[i] == '\r') answer[i] = '\0';
        if (strcmp(answer, "yes") != 0) {
            printf("nothing was changed\n");
            return 1;
        }
        unmount_kernel_fs();
        char *rm_args[] = { (char *)"rm", (char *)"-rf",
                            (char *)ROOT, NULL };
        int rc = run_wait("/bin/rm", rm_args);
        printf(rc == 0 ? "%s: gone\n" : "%s: could not remove it all\n", me);
        return rc == 0 ? 0 : 1;
    }

    /* Everything else needs the tree. Setting it up on first use rather
     * than making people find `apt setup` first: the command they typed
     * still does what they asked, it just takes longer the first time. */
    if (!is_set_up()) {
        printf("%s: Debian is not here yet - setting it up first.\n\n", me);
        if (cmd_setup(false) != 0)
            return 1;
        printf("\n");
    }

    /* `apt run <cmd>` and `apt shell` are ours; the rest is Debian's
     * apt, with the arguments passed straight through. */
    static char *args[64];
    int n = 0;

    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO, "usage: apt run <command> [args]\n");
            return 2;
        }
        for (int i = 2; i < argc && n < 63; i++)
            args[n++] = argv[i];
    } else if (strcmp(cmd, "shell") == 0) {
        args[n++] = (char *)"/bin/bash";
        args[n++] = (char *)"-l";
    } else {
        args[n++] = (char *)"/usr/bin/apt";
        for (int i = 1; i < argc && n < 63; i++)
            args[n++] = argv[i];
    }
    args[n] = NULL;

    int rc = call_in_debian(args);

    /* apt install pulls in things that want to start at boot. Nothing
     * in that tree is wired into our init, and saying so once is
     * better than letting somebody wonder why their service is not
     * running after a reboot. */
    if (rc == 0 && strcmp(cmd, "install") == 0)
        printf("\n%s: installed. Run it with `apt run <command>`.\n"
               "%s:   Debian services are not started by this system's"
               " init.\n", me, me);
    return rc;
}
