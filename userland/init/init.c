/* init.c - PID 1.
 *
 * Starting this program is the last thing the kernel does.
 * From here on it is our responsibility.
 *
 * The rules that only apply to PID 1:
 *   - It must never exit. If it does, the kernel panics.
 *   - Every orphaned process is reparented here. Left unreaped, zombies
 *     pile up until the process table is full.
 *   - It has to do the early setup itself: mount filesystems, open the
 *     console. What systemd would have done, we do. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SHELL_PATH   "/bin/sh"
#define RC_SCRIPT    "/etc/rc"
#define SERVICES     "/etc/services"
#define RESPAWN_MS   1000   /* pause before a respawn, to stop a busy loop */
#define MAX_SERVICES 8
#define MAX_SVC_ARGS 16

/* The virtual filesystems to mount.
 * Without these there is no ps and no /dev/null. */
static const struct {
    const char *source;
    const char *target;
    const char *fstype;
    unsigned long flags;
} MOUNTS[] = {
    { "proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC },
    { "sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC },
    /* With devtmpfs the kernel creates the device nodes itself.
     * It needs CONFIG_DEVTMPFS. */
    { "devtmpfs", "/dev",  "devtmpfs", MS_NOSUID },
    /* An SSH session needs a pseudo terminal. Without devpts, dropbear
     * cannot start a shell. This has to come after /dev. */
    { "devpts",   "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC },
};

static void mount_filesystems(void)
{
    for (unsigned i = 0; i < sizeof(MOUNTS) / sizeof(MOUNTS[0]); i++) {
        /* Create the mount point if it is missing from the initramfs. */
        if (!lp_is_dir(MOUNTS[i].target))
            lp_mkdir(MOUNTS[i].target, 0755);

        long r = lp_mount(MOUNTS[i].source, MOUNTS[i].target,
                          MOUNTS[i].fstype, MOUNTS[i].flags, NULL);
        if (r < 0)
            dprintf(STDERR_FILENO, "init: cannot mount %s (%ld)\n",
                    MOUNTS[i].target, -r);
    }
}

/* Attach the console to stdin, stdout and stderr.
 *
 * The kernel opens /dev/console as fd 0,1,2 before running init. But if
 * the initramfs has no /dev/console node that fails ("unable to open an
 * initial console") and init starts with no file descriptors at all - in
 * which case not even an error message can get out, and the cause is
 *
 * impossible to find. Once devtmpfs is mounted /dev/console exists, so we
 * open it ourselves. */
static void setup_console(void)
{
    long fd = lp_open("/dev/console", O_RDWR, 0);
    if (fd < 0)
        return;             /* if this fails there is no way to say so */

    lp_dup2((int)fd, STDIN_FILENO);
    lp_dup2((int)fd, STDOUT_FILENO);
    lp_dup2((int)fd, STDERR_FILENO);

    /* Tell the kernel's line editor that input is UTF-8.
     *
     * Without this, backspace erases a single byte. A Hangul character is
     * three bytes, so one press would leave a broken fragment instead of
     * removing the character. IUTF8 makes the kernel erase the whole thing. */
    lp_term_set_utf8((int)fd);

    if (fd > STDERR_FILENO)
        lp_close((int)fd);
}

/* Put the boot screen up.
 *
 * It runs as a separate program rather than as code in here: drawing
 * needs a font and some arithmetic, and none of that should be resident
 * in PID 1 for the life of the machine when it is wanted once, for a
 * second, at boot. Forking it costs a few milliseconds and it exits.
 *
 * Failure is normal and silent - a board with nothing plugged into HDMI
 * has no framebuffer, and there is nothing to say about that. */
static pid_t splash_pid = -1;

static void show_splash(void)
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return;

    if (pid == 0) {
        char *argv[] = { (char *)"splash", NULL };
        char *envp[] = { NULL };
        lp_execve("/bin/splash", argv, envp);
        lp_exit(127);
    }

    /* Deliberately not waited for. splash sits watching for /dev/fb0 to
     * turn up, which can take a second or two while the graphics device
     * is probed - and the boot has better things to do meanwhile. */
    splash_pid = pid;
}

/* Let the splash finish before anything else draws on the screen.
 *
 * They are drawing to the same pixels by two different routes - splash
 * straight into the framebuffer, the shell through the console driver -
 * and if the splash lands second it covers a prompt that is still live,
 * which is worse than no splash at all. By the time this is called the
 * boot script has already run, so normally there is nothing to wait for.
 * The three seconds is a ceiling, not a delay. */
static void wait_for_splash(void)
{
    if (splash_pid <= 0)
        return;

    for (int waited = 0; waited < 3000; waited += 100) {
        int   status = 0;
        pid_t r = lp_waitpid(splash_pid, &status, WNOHANG);
        if (r == splash_pid) {
            splash_pid = -1;
            return;
        }
        lp_sleep_ms(100);
    }
}

/* What the screen says when a shell starts on it.
 *
 * The serial console has seen every line of the boot; the screen has
 * seen the splash and whatever the kernel decided was important. Someone
 * sitting in front of it needs to be told where they are and what to
 * type, or a bare prompt is all they get. */
static void screen_greeting(int fd)
{
    dprintf(fd, "\x1b[H\x1b[2J");        /* clear, and go to the top */
    dprintf(fd, "  test_a_123_LPzero2W_img\n");
    dprintf(fd, "  Raspberry Pi Zero 2 W  -  built from scratch\n\n");
    dprintf(fd, "  help     every command there is\n");
    dprintf(fd, "  sysinfo  what this machine is and how it is doing\n");
    dprintf(fd, "  top      what is running\n\n");
    dprintf(fd, "  Tab completes. The arrow keys go back through what you\n");
    dprintf(fd, "  have typed. Files under /data and /root survive a reboot.\n\n");
}

static void banner(void)
{
    printf("\n");
    printf("  LP-zero OS\n");
    printf("  init (pid %d)\n", lp_getpid());
    printf("\n");
}

/* ── Supervising services ─────────────────────────────────────────
 *
 * This is what PID 1 is really for. Every line of /etc/services is started
 * as a program, and restarted when it dies.
 *
 * Why here and not in /etc/rc: rc runs once and finishes. Nothing revives
 * what it started. If the SSH server died that way, there would be no way
 * left to reach the machine. init already receives every child's exit, so
 * watching them here is the natural place. */

typedef struct {
    char   line[256];               /* the original; argv points into it */
    char  *argv[MAX_SVC_ARGS + 1];
    pid_t  pid;
    int    fails;                   /* consecutive failures */
} service_t;

static service_t services[MAX_SERVICES];
static int       nservices;

/* Split a line on whitespace into argv. Quotes are not handled - no
 * service command line has ever needed an argument with a space in it. */
static bool parse_service(const char *line, service_t *svc)
{
    strlcpy(svc->line, line, sizeof(svc->line));

    int n = 0;
    char *p = svc->line;

    while (*p && n < MAX_SVC_ARGS) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;

        svc->argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    svc->argv[n] = NULL;
    return n > 0;
}

static void start_service(service_t *svc)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed for %s\n", svc->argv[0]);
        svc->pid = -1;
        return;
    }

    if (pid == 0) {
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            NULL
        };
        lp_execve(svc->argv[0], svc->argv, envp);

        /* Search PATH ourselves: execve does not look at it. */
        char path[256];
        snprintf(path, sizeof(path), "/bin/%s", svc->argv[0]);
        lp_execve(path, svc->argv, envp);

        dprintf(STDERR_FILENO, "init: cannot run %s\n", svc->argv[0]);
        lp_exit(127);
    }

    svc->pid = pid;
}

static void load_services(void)
{
    /* Big enough for the file plus the comments explaining it. When this
     * was 2048 the file grew past it and the last service - the watchdog -
     * was silently cut off: it never started, and nothing said so. A
     * service quietly missing is the worst way for this to fail, so the
     * truncation is now reported. */
    char buf[8192];
    long n = proc_read(SERVICES, buf, sizeof(buf));
    if (n <= 0)
        return;

    if (n >= (long)sizeof(buf) - 1)
        dprintf(STDERR_FILENO,
                "init: %s is larger than %d bytes - the end was cut off\n",
                SERVICES, (int)sizeof(buf));

    char *p = buf;
    while (*p && nservices < MAX_SERVICES) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        /* Skip blank lines and comments. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            if (parse_service(p, &services[nservices])) {
                start_service(&services[nservices]);
                printf("init: started service %s (pid %d)\n",
                       services[nservices].argv[0],
                       (int)services[nservices].pid);
                nservices++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }
}

/* Restart a dead child if it is one we supervise. true if we handled it. */
static bool respawn_service(pid_t dead, int status)
{
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid != dead)
            continue;

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        dprintf(STDERR_FILENO, "init: service %s exited (code %d) - restarting\n",
                services[i].argv[0], code);

        /* Something that dies instantly would spin. Back off as failures add up. */
        services[i].fails++;
        long wait_ms = RESPAWN_MS * (services[i].fails > 5 ? 10 : 1);
        if (services[i].fails > 20) {
            dprintf(STDERR_FILENO,
                    "init:   %s keeps failing. Giving up on it\n",
                    services[i].argv[0]);
            services[i].pid = -1;
            return true;
        }

        lp_sleep_ms(wait_ms);
        start_service(&services[i]);
        return true;
    }
    return false;
}

/* Run the boot script and wait for it to finish.
 *
 * This lives in a script rather than in init because things like joining
 * WiFi or starting SSH change with the setup. Editing /etc/rc beats
 * rebuilding init every time. */
static void run_rc(void)
{
    if (!lp_exists(RC_SCRIPT))
        return;

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed for rc\n");
        return;
    }

    if (pid == 0) {
        char *argv[] = { (char *)SHELL_PATH, (char *)RC_SCRIPT, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            NULL
        };
        lp_execve(SHELL_PATH, argv, envp);
        lp_exit(127);
    }

    /* Wait for rc. Reap any other child that dies meanwhile. */
    for (;;) {
        int status = 0;
        pid_t done = lp_wait(&status);
        if (done < 0)
            break;
        if (done == pid) {
            if (!LP_WIFEXITED(status) || LP_WEXITSTATUS(status) != 0)
                dprintf(STDERR_FILENO, "init: %s exited with an error\n",
                        RC_SCRIPT);
            break;
        }
    }
}

/* Start one shell. Returns the child's pid. */
static pid_t spawn_shell_on(const char *tty)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed (%d)\n", (int)pid);
        return -1;
    }

    if (pid == 0) {
        /* Become the leader of a new session so we get a controlling
         * terminal. Without this, Ctrl-C and friends do nothing. */
        lp_setsid();

        /* A NULL tty means "inherit whatever init has", which is the
         * console the kernel picked. Otherwise open the named device and
         * make it this shell's stdin, stdout and stderr. */
        if (tty) {
            long fd = lp_open(tty, O_RDWR, 0);
            if (fd < 0)
                lp_exit(127);
            lp_dup2((int)fd, STDIN_FILENO);
            lp_dup2((int)fd, STDOUT_FILENO);
            lp_dup2((int)fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                lp_close((int)fd);
            lp_term_set_utf8(STDIN_FILENO);

            /* Only the screen gets a greeting. On the serial console the
             * boot log is right there above the prompt and says more
             * than any greeting would. */
            screen_greeting(STDOUT_FILENO);
        }

        char *argv[] = { (char *)SHELL_PATH, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            (char *)"TERM=linux",
            NULL
        };

        lp_execve(SHELL_PATH, argv, envp);

        dprintf(STDERR_FILENO, "init: cannot run %s\n", SHELL_PATH);
        lp_exit(127);
    }

    return pid;
}

static pid_t spawn_shell(void) { return spawn_shell_on(NULL); }

/* Is the screen a separate place from the console we already talk to?
 *
 * The kernel picks one device as /dev/console - the last console= on the
 * command line. Ours puts the serial port last, because that is what a
 * headless board and an SSH session need.
 *
 * But in a VM with a window (UTM, or QEMU with a display), the screen is
 * tty1 and it would then show the boot messages and nothing else: the
 * shell would be off on the serial port, and the window would look dead.
 *
 * /sys/class/tty/console/active lists the active consoles, and the LAST
 * one is where /dev/console actually points. Being in the list is not
 * enough: with "console=tty1 console=ttyAMA0" both are listed, yet the
 * shell only lands on ttyAMA0. So we compare against the last entry, and
 * put a second shell on tty1 when that is not it. Costs one idle
 * process, and it means an HDMI screen plugged in later already has a
 * shell waiting on it. */
static bool screen_needs_its_own_shell(void)
{
    if (!lp_exists("/dev/tty1"))
        return false;

    char buf[128];
    long fd = lp_open("/sys/class/tty/console/active", O_RDONLY, 0);
    if (fd < 0)
        return false;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    /* Find the last whitespace-separated name. */
    const char *last = buf;
    for (char *p = buf; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            *p = '\0';
            if (p[1] && p[1] != ' ' && p[1] != '\n' && p[1] != '\t')
                last = p + 1;
        }
    }

    return strcmp(last, "tty1") != 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    bool is_pid1 = (lp_getpid() == 1);

    /* When we are not PID 1 (run by hand during development) skip the
     * mounts - remounting over a live system would not end well. */
    if (is_pid1) {
        mount_filesystems();
        setup_console();
        show_splash();
    } else
        printf("init: not pid 1, skipping the mounts (test mode)\n");

    banner();

    if (is_pid1) {
        run_rc();
        load_services();
    }

    pid_t shell_pid = spawn_shell();

    /* A VM window, or an HDMI screen, is a console nobody is reading
     * unless we put a shell there too. */
    pid_t screen_pid = -1;
    if (is_pid1 && screen_needs_its_own_shell()) {
        wait_for_splash();
        screen_pid = spawn_shell_on("/dev/tty1");
        if (screen_pid > 0)
            printf("init: a second shell is on the screen (tty1)\n");
    }

    /* Main loop: reap dead children, and restart the shell when it exits. */
    for (;;) {
        int status = 0;
        pid_t pid = lp_wait(&status);

        if (pid < 0) {
            /* No children to wait for (ECHILD = 10). PID 1 must not exit,
             * so pause and look again. */
            if (!is_pid1)
                break;
            lp_sleep_ms(200);
            continue;
        }

        if (respawn_service(pid, status))
            continue;           /* it was a supervised service */

        if (pid == screen_pid) {
            lp_sleep_ms(RESPAWN_MS);
            screen_pid = spawn_shell_on("/dev/tty1");
            continue;
        }

        if (pid != shell_pid)
            continue;           /* just an orphan we reaped */

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        printf("\ninit: the shell exited (code %d). Starting it again.\n", code);

        if (!is_pid1)
            break;              /* in test mode, only once */

        lp_sleep_ms(RESPAWN_MS);
        shell_pid = spawn_shell();
    }

    printf("init: exiting\n");
    return 0;
}
