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
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SHELL_PATH   "/bin/sh"
#define RC_SCRIPT    "/etc/rc"
#define SERVICES     "/etc/services"
#define RESPAWN_MS   1000   /* pause before a respawn, to stop a busy loop */
#define MAX_SERVICES 16
#define SERVICE_PIDS "/var/service.pids"
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
    dprintf(fd, "  " LP_OS_TITLE "\n");
    dprintf(fd, "  " LP_OS_MACHINE "\n\n");
    dprintf(fd, "  help     every command there is\n");
    dprintf(fd, "  sysinfo  what this machine is and how it is doing\n");
    dprintf(fd, "  top      what is running\n\n");
    dprintf(fd, "  Tab completes. The arrow keys go back through what you\n");
    dprintf(fd, "  have typed. Files under /data and /root survive a reboot.\n\n");
}

static void banner(void)
{
    printf("\n");
    printf("  " LP_OS_NAME " OS\n");
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
    s64    retry_at;                /* monotonic ms to start it again, 0 = none */
    s64    started_at;              /* monotonic ms, to tell a crash loop from
                                       a service that ran for a week */
    bool   given_up;                /* failed too often; not tried again */
    bool   was_off;                 /* it was in the disabled list last look */
    bool   off_refused;             /* said once: critical, listed, started anyway */
} service_t;

static service_t services[MAX_SERVICES];
static int       nservices;

/* ── Services this machine cannot be left without ──
 *
 * Everything in /etc/services gets restarted when it dies. These three
 * are also never given up on, however often they fail, and their backoff
 * is capped short.
 *
 *   guard     is the whole self-defence of the machine: the memory
 *             killer, the disk-full check, the CPU-hog demotion. With
 *             guard gone the board looks fine and is defenceless, and
 *             the next thing that goes wrong takes it down with nothing
 *             to stop it or say so. It is the one process whose absence
 *             is invisible, which is exactly why it must not stay absent.
 *   dropbear  is the only way in.
 *   watchdog  is the only way back from a wedge.
 *
 * "Giving up" is right for a service that is broken - it stops a restart
 * loop from being the thing that kills the board. It is wrong for these
 * three, because a board with none of them running is not a board worth
 * protecting from a restart loop. The short backoff is what makes the
 * loop affordable. */
static const char *CRITICAL[] = { "guard", "dropbear", "watchdog", NULL };

static bool is_critical(const char *name)
{
    for (int i = 0; CRITICAL[i]; i++)
        if (strcmp(name, CRITICAL[i]) == 0)
            return true;
    return false;
}

/* ── Telling guard which pids to protect ──
 *
 * guard decides what it may kill. It used to decide by process name, and
 * a name is whatever the process says it is: anything called "dropbear"
 * was safe from the memory killer, and anything called "sh" was safe
 * from both that and the CPU-hog demotion - which made every runaway
 * started from a shell immune to the only thing watching for runaways.
 *
 * A pid cannot be forged. init is the one process that knows the pids of
 * the things it supervises, so it writes them here and guard reads them.
 * /var is on the root filesystem, which is RAM inside the kernel image
 * and writable by root alone, so nothing unprivileged can plant a pid in
 * this file and become unkillable. */
static bool pids_dirty = true;

static void publish_service_pids(void)
{
    long fd = lp_open(SERVICE_PIDS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;             /* guard falls back to its built-in list */
    for (int i = 0; i < nservices; i++)
        if (services[i].pid > 0)
            dprintf((int)fd, "%d %s\n",
                    (int)services[i].pid, services[i].argv[0]);
    lp_close((int)fd);
}

/* Split a line on whitespace into argv. Quotes are not handled - no
 * service command line has ever needed an argument with a space in it. */
/* ── '?<path>' : only if that path exists ──
 *
 * Some services only make sense on some machines. wpa_supplicant is the
 * case that forced this: on a real Pi it drives the WiFi, and in a
 * virtual machine there is no wlan0, so it fails instantly, and init
 * restarts it, backs off, restarts it, twelve times, printing a driver
 * error each round before giving up. A supervisor doing exactly what it
 * is meant to do, about hardware that is not there and never will be.
 *
 * "?/sys/class/net/wlan0 wpa_supplicant -i wlan0 ..." says the
 * condition out loud instead. No condition, no change: everything else
 * starts as it always did. */
static bool service_wanted_here(const char *line, const char **rest)
{
    *rest = line;
    if (*line != '?')
        return true;

    line++;
    char path[128];
    size_t i = 0;
    while (*line && *line != ' ' && *line != '\t' && i < sizeof path - 1)
        path[i++] = *line++;
    path[i] = '\0';

    while (*line == ' ' || *line == '\t')
        line++;
    *rest = line;

    if (lp_exists(path))
        return true;

    printf("init: not starting %s - there is no %s on this machine\n",
           line, path);
    return false;
}

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
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
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
    pids_dirty = true;
}

/* Safe mode: lpzero.safe on the kernel command line.
 *
 * The boot partition is FAT32 and cmdline.txt is a text file on it, so
 * this is one word typed from any PC with a card reader - which is the
 * only tool available when a machine will not come up far enough to log
 * into. In safe mode init starts SSH and nothing else, and /etc/rc
 * skips the user's startup script.
 *
 * SSH rather than nothing at all, because a safe mode you cannot reach
 * is only a slower way of pulling the card out. */
static bool safe_mode(void)
{
    static bool checked = false;
    static bool safe    = false;

    if (checked)
        return safe;
    checked = true;

    char cmdline[1024];
    if (proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) > 0)
        safe = (strstr(cmdline, "lpzero.safe") != NULL);
    return safe;
}

/* In safe mode, which services still start.
 *
 * Only the SSH server. Not ntp - a wrong clock does not stop anyone
 * logging in. Not logd - it writes to the data partition, which may be
 * the thing that is wrong. Not the watchdog - if the machine is being
 * rescued, a reset every fifteen seconds is in the way. */
static bool wanted_in_safe_mode(const char *program)
{
    return strcmp(program, "dropbear") == 0;
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
            const char *cmd = p;
            if (!service_wanted_here(p, &cmd)) {
                if (!eol) break;
                p = eol + 1;
                continue;
            }
            if (parse_service(cmd, &services[nservices])) {
                if (safe_mode() &&
                    !wanted_in_safe_mode(services[nservices].argv[0])) {
                    printf("init: safe mode - not starting %s\n",
                           services[nservices].argv[0]);
                    if (!eol) break;
                    p = eol + 1;
                    continue;
                }
                start_service(&services[nservices]);
                services[nservices].started_at = lp_monotonic_ms();
                printf("init: started service %s (pid %d)\n",
                       services[nservices].argv[0],
                       (int)services[nservices].pid);
                nservices++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (pids_dirty) {
        publish_service_pids();
        pids_dirty = false;
    }

    /* One more thing that used to be silent: more lines than there is
     * room for meant the last services never started and nothing said
     * so. The file is read in full above; this is the table filling up. */
    if (nservices >= MAX_SERVICES)
        dprintf(STDERR_FILENO,
                "init: ** %s has more than %d services."
                " The rest were not started.\n", SERVICES, MAX_SERVICES);
}

#define DISABLED_FILE  "/data/services.disabled"

/* ── Turning a service off ────────────────────────────────────────────
 *
 * /data/services.disabled is a list of names, one per line, that init
 * will not start. `service stop x` writes the name there and kills the
 * process; init sees it die, finds it in the list, and leaves it alone.
 * `service start x` takes the line out, and init starts it again on its
 * next look.
 *
 * A file rather than a signal, because there is no way to send init a
 * message here - no handler support in the libc, and pid 1 catching
 * signals is its own set of problems. A file is also the thing you can
 * read to find out why something is not running, which a signal is not.
 *
 * It is on /data, so a service turned off stays off across a reboot.
 * Delete the file and everything comes back. */
static bool service_disabled(const char *name)
{
    long fd = lp_open(DISABLED_FILE, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[128];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\r'))
            *--end = '\0';
        if (line[0] && strcmp(line, name) == 0) { found = true; break; }
    }
    lp_close((int)fd);
    return found;
}

/* Note that a supervised service died, and when to try it again.
 * true if it was one of ours.
 *
 * The restart is scheduled, not slept through. init used to sleep here,
 * which meant that while one service was backing off, pid 1 was not
 * reaping anything or restarting the shell - the whole machine waited
 * on the one thing that was already broken.
 *
 * The delay doubles each time, up to half a minute. Without that, a
 * service that cannot possibly start - the watchdog daemon on a board
 * with no watchdog - takes the console with it: twenty restarts and
 * forty lines of the same message, in the second and a half it takes
 * anyone to read the first one. */
static bool respawn_service(pid_t dead, int status)
{
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid != dead)
            continue;

        services[i].pid = -1;
        pids_dirty = true;

        if (service_disabled(services[i].argv[0])) {
            printf("init: service %s stopped (it is in %s)\n",
                   services[i].argv[0], DISABLED_FILE);
            services[i].retry_at = 0;
            services[i].fails    = 0;
            return true;
        }

        /* A service that stayed up for a minute was not failing to
         * start - it died of something else. Do not make it serve a
         * backoff earned months ago. */
        if (lp_monotonic_ms() - services[i].started_at > 60000)
            services[i].fails = 0;
        services[i].fails++;

        /* "Nothing for me to do on this machine." Not a failure, and
         * not something that changes while the machine is running, so
         * asking again - twelve times, with a growing backoff, printing
         * the same thing each time - is pure noise. The watchdog service
         * on a virtual machine is the case this exists for. */
        if (LP_WIFEXITED(status) &&
            LP_WEXITSTATUS(status) == LP_EXIT_NO_HARDWARE) {
            printf("init: %s has nothing to do on this machine"
                   " - not starting it again\n", services[i].argv[0]);
            services[i].retry_at = 0;
            services[i].fails    = 0;
            services[i].given_up = true;
            return true;
        }

        bool critical = is_critical(services[i].argv[0]);

        if (services[i].fails > 12 && !critical) {
            dprintf(STDERR_FILENO,
                    "init:   %s keeps failing. Giving up on it.\n"
                    "init:   'service start %s' tries again.\n",
                    services[i].argv[0], services[i].argv[0]);
            services[i].retry_at = 0;
            services[i].given_up = true;
            return true;
        }

        long cap = critical ? 5000 : 30000;
        long wait_ms = RESPAWN_MS;
        for (int n = 1; n < services[i].fails && wait_ms < cap; n++)
            wait_ms *= 2;
        if (wait_ms > cap) wait_ms = cap;

        /* Say it plainly rather than quietly retrying forever: a
         * critical service that has failed a dozen times is a fault
         * somebody has to look at, even though init will keep trying. */
        if (critical && services[i].fails == 13)
            dprintf(STDERR_FILENO,
                    "init:   ** %s has failed 12 times and is critical,"
                    " so init keeps trying.\n"
                    "init:   ** Something is wrong with it. This machine"
                    " is running without it.\n",
                    services[i].argv[0]);

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        dprintf(STDERR_FILENO,
                "init: service %s exited (code %d) - again in %lds\n",
                services[i].argv[0], code, wait_ms / 1000);
        {
            char m[160];
            snprintf(m, sizeof m, "service %s exited (code %d), restarting",
                     services[i].argv[0], code);
            lp_log("init", m);
        }

        services[i].retry_at = lp_monotonic_ms() + wait_ms;
        return true;
    }
    return false;
}

/* Start whatever is due. Called every time round the main loop. */
static void restart_due_services(void)
{
    s64 now = lp_monotonic_ms();
    for (int i = 0; i < nservices; i++) {
        service_t *s = &services[i];

        /* ── Why is_critical() is checked HERE and not in `service` ──
         *
         * `service stop guard` refuses without --force. That check lives
         * in the service program, which is the wrong process for it:
         * the file it guards is an ordinary line of text in /data, and
         * anything that can write a file can append to it. Two commands
         * -   echo guard >> /data/services.disabled
         *     kill guard
         * - reproduced exactly the failure the supervision was added to
         * stop, and left guard down for the rest of the uptime.
         *
         * A refusal is only real where the decision is made. init is the
         * only reader that acts on this file, so the answer belongs
         * here: the three services the board cannot defend itself
         * without are restarted whatever the file says. `service` still
         * asks for --force, but that is now a courtesy to the person
         * typing, not the enforcement. */
        if (service_disabled(s->argv[0])) {
            if (!is_critical(s->argv[0])) {
                s->was_off = true;
                continue;               /* somebody turned it off */
            }
            if (!s->off_refused) {
                s->off_refused = true;
                printf("init: %s is listed in %s, but it is one of the"
                       " services this board cannot defend itself"
                       " without.\n"
                       "init:   Starting it anyway. Remove the line to"
                       " stop seeing this.\n",
                       s->argv[0], DISABLED_FILE);
            }
        }

        /* Coming back from being turned off clears the failure history.
         * Whoever re-enabled it is asking for a fresh try, not for the
         * backoff it had earned before. */
        if (s->was_off) {
            s->was_off  = false;
            s->fails    = 0;
            s->given_up = false;
            s->retry_at = 0;
        }

        if (s->pid > 0)
            continue;
        if (s->given_up)
            continue;                   /* it said so already */
        if (s->retry_at != 0 && now < s->retry_at)
            continue;                   /* still serving its backoff */

        s->retry_at = 0;
        start_service(s);
        s->started_at = lp_monotonic_ms();
        printf("init: started service %s (pid %d)\n",
               s->argv[0], (int)s->pid);
    }

    /* Only when a pid actually changed. This file is read by guard to
     * decide what it must not kill, so it has to be right, but rewriting
     * it every time round the loop would be a write a second forever. */
    if (pids_dirty) {
        publish_service_pids();
        pids_dirty = false;
    }
}

/* Is anything waiting to be restarted? Only then does the main loop have
 * to poll instead of blocking in wait(). */
static bool services_pending(void)
{
    /* A service waiting out a backoff needs looking at again, and so
     * does one that somebody may be about to re-enable - blocking in
     * wait() would mean `service start` did nothing until the next time
     * some other process happened to exit.
     *
     * One that init has given up on does not: it will not be started
     * again until somebody says so, and that somebody takes it out of
     * the disabled list, which is the was_off path. Polling for it
     * forever would be a wakeup a second for the life of the machine. */
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid > 0)
            continue;
        if (services[i].retry_at != 0)
            return true;
        if (services[i].was_off)
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
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
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
/* Defined below, next to the screen-shell decision that also uses it. */
static bool console_device(char *out, size_t size);

static pid_t spawn_shell_on(const char *tty, bool greet)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed (%d)\n", (int)pid);
        return -1;
    }

    if (pid == 0) {
        /* Become the leader of a new session. This is half of getting a
         * controlling terminal; the other half is asking for one, below,
         * and both halves are needed. With neither, the terminal has no
         * foreground process group, the line discipline has nobody to
         * send a signal to, and Ctrl-C is silently nothing - you type it
         * at a `cat` waiting on input and the 0x03 goes in as data. */
        lp_setsid();

        /* A NULL tty means "inherit whatever init has". Otherwise open
         * the named device and make it this shell's stdin, stdout and
         * stderr. */
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
        }

        /* Claim it. Without this a shell on the console has no
         * controlling terminal, and every key that is supposed to be a
         * signal - Ctrl-C, Ctrl-\, Ctrl-Z - is just a byte.
         *
         * It has to be the real device: /dev/console can never be a
         * controlling terminal, because the kernel forces O_NOCTTY for
         * it. That is why the caller resolves the device name first
         * rather than letting the shell inherit init's own fds.
         *
         * Not fatal if it fails - a shell with no signals still beats no
         * shell - but it is worth saying, because everything downstream
         * of it looks like the shell ignoring the keyboard. */
        if (lp_term_make_controlling(STDIN_FILENO) < 0)
            dprintf(STDERR_FILENO,
                    "init: could not claim %s as the controlling"
                    " terminal - Ctrl-C will not work there\n",
                    tty ? tty : "the console");

        if (greet)
            screen_greeting(STDOUT_FILENO);

        char *argv[] = { (char *)SHELL_PATH, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
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

/* The shell on whatever the console is.
 *
 * It used to be spawn_shell_on(NULL) - inherit init's own fds, which are
 * /dev/console. That is why Ctrl-C did nothing on this machine: a shell
 * whose terminal is /dev/console cannot have a controlling terminal, so
 * no key ever became a signal. Resolving the device by name and opening
 * that instead is the whole fix. */
static pid_t spawn_shell(void)
{
    char name[64], path[80];
    if (console_device(name, sizeof name)) {
        snprintf(path, sizeof path, "/dev/%s", name);
        if (lp_exists(path))
            return spawn_shell_on(path, false);
    }

    /* No /sys yet, or a console with no device node. The shell still
     * runs; it just has no signals, which is what it always had. */
    return spawn_shell_on(NULL, false);
}

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
/* The name of the device /dev/console actually points at - "ttyAMA0",
 * "tty1" - or false when it cannot be worked out.
 *
 * This is needed for more than deciding where to put a second shell.
 * A shell on /dev/console has no controlling terminal and can never
 * have one: the kernel refuses to make major 5 minor 1 a controlling
 * terminal, whatever you open it with. So the shell has to be given the
 * real device, by name, or Ctrl-C on the console does nothing at all -
 * no signal is generated, because a terminal with no foreground process
 * group has nobody to signal. */
static bool console_device(char *out, size_t size)
{
    char buf[128];
    long fd = lp_open("/sys/class/tty/console/active", O_RDONLY, 0);
    if (fd < 0)
        return false;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    /* Find the last whitespace-separated name. With
     * "console=tty1 console=ttyAMA0" both are listed and the last one
     * is where /dev/console goes. */
    const char *last = buf;
    for (char *p = buf; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            *p = '\0';
            if (p[1] && p[1] != ' ' && p[1] != '\n' && p[1] != '\t')
                last = p + 1;
        }
    }
    if (!*last)
        return false;

    strlcpy(out, last, size);
    return true;
}

static bool screen_needs_its_own_shell(void)
{
    if (!lp_exists("/dev/tty1"))
        return false;

    char name[64];
    if (!console_device(name, sizeof name))
        return false;

    return strcmp(name, "tty1") != 0;
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

    if (is_pid1 && safe_mode()) {
        printf("  ** SAFE MODE **  (lpzero.safe on the kernel command line)\n");
        printf("  SSH only. /data/rc.local is skipped.\n");
        printf("  Take the word out of cmdline.txt to boot normally.\n\n");
    }

    if (is_pid1) {
        run_rc();
        load_services();
    }

    pid_t shell_pid = spawn_shell();

    /* A VM window, or an HDMI screen, is a console nobody is reading
     * unless we put a shell there too. */
    /* A shell on the screen with no login prompt.
     *
     * That is right for a board on a desk being worked on, and wrong for
     * one in a public place: plugging in a monitor and a keyboard gives
     * whoever did it root, immediately. There is no password to ask for -
     * password authentication is compiled out of this system entirely -
     * so the only choice is whether the shell is there at all.
     *
     * lpzero.noscreen on the kernel command line turns it off. The
     * serial console and SSH are unaffected. cmdline.txt is on the boot
     * partition, so this is one word typed from any PC. */
    bool no_screen = false;
    {
        char cmdline[1024];
        if (proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) > 0)
            no_screen = (strstr(cmdline, "lpzero.noscreen") != NULL);
    }

    pid_t screen_pid = -1;
    if (is_pid1 && no_screen) {
        printf("init: no shell on the screen (lpzero.noscreen)\n");
    } else if (is_pid1 && screen_needs_its_own_shell()) {
        wait_for_splash();
        screen_pid = spawn_shell_on("/dev/tty1", true);
        if (screen_pid > 0)
            printf("init: a second shell is on the screen (tty1)\n");
    }

    /* Main loop: reap dead children, restart services whose backoff has
     * run out, and restart the shell when it exits.
     *
     * The wait does not block: a service waiting to be restarted has a
     * time to be restarted at, and nothing would notice it passing if
     * pid 1 were parked in wait(). */
    for (;;) {
        restart_due_services();

        int status = 0;
        bool pending = services_pending();
        pid_t pid = pending ? lp_waitpid(-1, &status, WNOHANG)
                            : lp_wait(&status);

        if (pid == 0) {
            /* Children, but none of them finished, and something is not
             * running. Look again shortly - a second is soon enough for
             * `service start` to feel immediate and rare enough to cost
             * nothing. */
            lp_sleep_ms(1000);
            continue;
        }

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
            screen_pid = spawn_shell_on("/dev/tty1", true);
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
