/* service - what init is keeping alive, and turning it on or off.
 *
 *   service                  everything, and whether it is running
 *   service status <name>
 *   service restart <name>
 *   service stop <name>
 *   service start <name>
 *
 * ── How this works, which is not how systemd works ──
 * init reads /etc/services at boot and restarts anything in it that
 * dies. That is the whole supervisor: no units, no dependencies, no
 * targets, no sockets. This command talks to it through two things -
 * signals, and a file.
 *
 *   restart   kill it. init notices and starts it again. That is not a
 *             workaround; it is what "restart" means here.
 *   stop      write the name into /data/services.disabled, then kill
 *             it. init finds it in that file and leaves it alone.
 *   start     take the name out of the file. init looks about once a
 *             second when anything is down, so it comes back.
 *
 * The file is on the data partition, so a service turned off stays off
 * across a reboot. It is also readable, which is the answer to "why is
 * this not running" that a signal could never be.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define SERVICES  "/etc/services"
#define DISABLED  "/data/services.disabled"

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* ── which services there are ────────────────────────────────────── */

#define MAX_SERVICES 16
static char names[MAX_SERVICES][32];
static char lines[MAX_SERVICES][160];
static int  nservices = 0;

static void load(void)
{
    long fd = lp_open(SERVICES, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];
    while (nservices < MAX_SERVICES &&
           readline((int)fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#')
            continue;

        /* A line may open with "?<path>", meaning init only starts it
         * where that path exists. Skip the condition before taking the
         * name, or the service is called "?/sys/class/net/wlan0" and
         * every command that takes a name stops working for it. That is
         * how `service restart wpa_supplicant` came to answer "not in
         * /etc/services" - on a board where WiFi is the only way in,
         * and restarting it is the first thing you reach for. */
        if (*p == '?') {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p)
                continue;              /* a condition and nothing else */
        }

        strlcpy(lines[nservices], p, sizeof lines[0]);

        char *end = p;
        while (*end && *end != ' ' && *end != '\t') end++;
        size_t len = (size_t)(end - p);
        if (len >= sizeof names[0]) len = sizeof names[0] - 1;
        memcpy(names[nservices], p, len);
        names[nservices][len] = '\0';
        nservices++;
    }
    lp_close((int)fd);
}

static int index_of(const char *name)
{
    for (int i = 0; i < nservices; i++)
        if (strcmp(names[i], name) == 0)
            return i;
    return -1;
}

/* ── is it running ───────────────────────────────────────────────── */

/* The first pid running under this name, or 0.
 *
 * /proc/<pid>/comm is the program name and nothing else - no path, no
 * arguments - so a match cannot be some other process that merely
 * mentions the name on its command line. */
static int pid_of(const char *name)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    int  found = 0;

    while (!found) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n && !found; ) {
            char *rec = buf + off;
            u16   len = *(u16 *)(rec + DIRENT_RECLEN);
            char *ent = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;

            int pid = atoi(ent);
            if (pid <= 1)
                continue;

            char path[64], comm[64];
            snprintf(path, sizeof path, "/proc/%d/comm", pid);
            if (proc_read(path, comm, sizeof comm) <= 0)
                continue;
            for (int j = 0; comm[j]; j++)
                if (comm[j] == '\n') { comm[j] = '\0'; break; }
            if (strcmp(comm, name) == 0)
                found = pid;
        }
    }
    lp_close((int)fd);
    return found;
}

/* ── the disabled list ───────────────────────────────────────────── */

static bool is_disabled(const char *name)
{
    long fd = lp_open(DISABLED, O_RDONLY, 0);
    if (fd < 0)
        return false;
    char line[128];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0)
        if (line[0] && strcmp(line, name) == 0) { found = true; break; }
    lp_close((int)fd);
    return found;
}

static int set_disabled(const char *name, bool off)
{
    /* Read what is there, write it back with this name added or gone.
     * The list is a handful of short lines; there is nothing to stream. */
    static char kept[2048];
    size_t used = 0;
    bool  present = false;

    long fd = lp_open(DISABLED, O_RDONLY, 0);
    if (fd >= 0) {
        char line[128];
        while (readline((int)fd, line, sizeof line) >= 0) {
            if (!line[0])
                continue;
            if (strcmp(line, name) == 0) { present = true; continue; }
            size_t l = strlen(line);
            if (used + l + 2 < sizeof kept) {
                memcpy(kept + used, line, l);
                used += l;
                kept[used++] = '\n';
            }
        }
        lp_close((int)fd);
    }

    if (off == present && off)
        return 0;                       /* already off */

    if (off) {
        size_t l = strlen(name);
        if (used + l + 2 < sizeof kept) {
            memcpy(kept + used, name, l);
            used += l;
            kept[used++] = '\n';
        }
    }

    long out = lp_open(DISABLED, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO,
                "service: cannot write %s (%ld).\n"
                "service:   Without the data partition there is nowhere to\n"
                "service:   remember this, so it would come back anyway.\n",
                DISABLED, -out);
        return 1;
    }
    if (used)
        lp_write((int)out, kept, used);
    lp_close((int)out);
    lp_sync();
    return 0;
}

/* ── the commands ────────────────────────────────────────────────── */

static int list_all(void)
{
    printf("  %-16s %8s  %s\n", "service", "pid", "state");
    for (int i = 0; i < nservices; i++) {
        int pid = pid_of(names[i]);
        const char *state = pid ? "running"
                          : is_disabled(names[i]) ? "stopped (by hand)"
                          : "not running";
        char pidbuf[12] = "-";
        if (pid) snprintf(pidbuf, sizeof pidbuf, "%d", pid);
        printf("  %-16s %8s  %s\n", names[i], pidbuf, state);
    }
    printf("\n  init restarts anything here that dies. %s\n", SERVICES);
    printf("  is the list; a service turned off is remembered in\n");
    printf("  %s and stays off across a reboot.\n", DISABLED);
    return 0;
}

static int status_of(const char *name)
{
    int i = index_of(name);
    if (i < 0) {
        dprintf(STDERR_FILENO,
                "service: %s is not in %s.\n"
                "service:   'service' with no arguments lists what is.\n",
                name, SERVICES);
        return 1;
    }
    int pid = pid_of(name);
    printf("%s\n", names[i]);
    printf("  command  %s\n", lines[i]);
    if (pid) {
        char path[64], buf[64];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        printf("  running  pid %d\n", pid);
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        if (proc_read(path, buf, sizeof buf) > 0) { /* first lines only */ }
    } else if (is_disabled(name)) {
        printf("  stopped  by hand - it is in %s\n", DISABLED);
        printf("           'service start %s' brings it back\n", name);
    } else {
        printf("  stopped  and not on purpose. init gives up on a service\n");
        printf("           that keeps failing; 'dmesg' and the console say\n");
        printf("           why.\n");
    }
    return pid ? 0 : 1;
}

static int do_restart(const char *name)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }
    /* Make sure it is not disabled, or killing it would stop it for
     * good and "restart" would have meant "stop". */
    set_disabled(name, false);

    int pid = pid_of(name);
    if (!pid) {
        printf("service: %s was not running. init will start it.\n", name);
        return 0;
    }
    if (lp_kill(pid, SIGTERM) < 0) {
        dprintf(STDERR_FILENO, "service: cannot signal pid %d\n", pid);
        return 1;
    }
    printf("service: stopped %s (pid %d). init starts it again.\n", name, pid);
    return 0;
}

/* ── Stopping the things that keep the machine alive ──
 *
 * `service stop x` writes x to /data/services.disabled, which is on the
 * data partition and therefore survives a reboot. For these three that
 * is not "stop it", it is "disable it permanently, on a board that may
 * be somewhere else by the time it matters":
 *
 *   guard     every defence the machine has - the memory killer, the
 *             fork-storm response, the disk-full check. Without it the
 *             board looks healthy right up to the moment it is gone.
 *   dropbear  the only way in.
 *   watchdog  the only way back from a wedge.
 *
 * --force still does it, because somebody debugging guard needs to be
 * able to stop guard. It just cannot happen by typing four words. */
static bool is_critical_service(const char *name)
{
    return strcmp(name, "guard") == 0
        || strcmp(name, "dropbear") == 0
        || strcmp(name, "watchdog") == 0;
}

static int do_stop(const char *name, bool force)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }

    if (is_critical_service(name) && !force) {
        dprintf(STDERR_FILENO,
                "service: %s is not something to stop.\n", name);
        if (strcmp(name, "guard") == 0)
            dprintf(STDERR_FILENO,
                    "service:   It is the whole self-defence of this"
                    " machine: what kills a runaway\n"
                    "service:   before it takes the board down, what"
                    " notices /data filling up, what\n"
                    "service:   keeps SSH answering under load. Stopping"
                    " it leaves a board that\n"
                    "service:   looks fine and is defenceless, and"
                    " nothing to say so afterwards.\n");
        else if (strcmp(name, "dropbear") == 0)
            dprintf(STDERR_FILENO,
                    "service:   It is the only way into this machine.\n");
        else
            dprintf(STDERR_FILENO,
                    "service:   It is the only thing that can reboot a"
                    " board that has wedged.\n");
        dprintf(STDERR_FILENO,
                "service:\n"
                "service:   This would also outlast a reboot - the"
                " disabled list is on /data.\n"
                "service:   'service stop %s --force' if you mean it.\n",
                name);
        return 1;
    }

    /* Written before the kill, not after. The other order is a race:
     * init sees the death, finds nothing in the file, and starts it
     * again before this process gets to write. */
    if (set_disabled(name, true) != 0)
        return 1;

    int pid = pid_of(name);
    if (pid)
        lp_kill(pid, SIGTERM);

    printf("service: %s is stopped and will stay stopped, including after\n"
           "         a reboot. 'service start %s' undoes it.\n", name, name);

    if (strcmp(name, "dropbear") == 0)
        printf("\nservice:   ** that was the SSH server. If you are reading\n"
               "service:      this over SSH, this session is the last one.\n");
    return 0;
}

static int do_start(const char *name)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }
    if (pid_of(name)) {
        printf("service: %s is already running.\n", name);
        return 0;
    }
    if (set_disabled(name, false) != 0)
        return 1;
    printf("service: %s enabled. init starts it within a second.\n", name);
    return 0;
}

static void usage(void)
{
    printf("service - what init is keeping alive\n\n");
    printf("  service                    all of them, and their state\n");
    printf("  service status <name>\n");
    printf("  service restart <name>\n");
    printf("  service stop <name>        and keep it stopped\n");
    printf("  service start <name>\n\n");
    printf("init reads %s at boot and restarts anything in it\n", SERVICES);
    printf("that dies. There are no units, dependencies or targets - it is\n");
    printf("a list of commands and a supervisor.\n\n");
    printf("'restart' kills it and lets init do its job. 'stop' writes the\n");
    printf("name into %s first, which is what stops\n", DISABLED);
    printf("init bringing it back, and is also the file to read when you\n");
    printf("want to know why something is not running.\n");
}

int main(int argc, char **argv)
{
    load();

    if (nservices == 0) {
        dprintf(STDERR_FILENO,
                "service: %s is empty or missing, so init is\n"
                "service:   supervising nothing.\n", SERVICES);
        return 1;
    }

    if (argc < 2)
        return list_all();

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) { usage(); return 0; }
    if (strcmp(cmd, "list") == 0) return list_all();

    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: service %s <name>\n", cmd);
        return 2;
    }
    const char *name = argv[2];

    bool force = false;
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            force = true;

    if (strcmp(cmd, "status") == 0)  return status_of(name);
    if (strcmp(cmd, "restart") == 0) return do_restart(name);
    if (strcmp(cmd, "stop") == 0)    return do_stop(name, force);
    if (strcmp(cmd, "start") == 0)   return do_start(name);

    dprintf(STDERR_FILENO, "service: no idea what \"%s\" means\n", cmd);
    usage();
    return 2;
}
