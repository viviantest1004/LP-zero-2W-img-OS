/* memwatch - the memory safety net.
 *
 * Linux already has an OOM killer. Two things make it hard to rely on:
 *   1) It acts too late. By the time an allocation actually fails the
 *      machine has already been crawling for a while.
 *   2) We do not choose what it kills. If the shell or the SSH server
 *      goes first, there is no way left to reach the machine.
 *
 * So we step in earlier, on our own thresholds:
 *   free < WARN_MB     -> warn on the console
 *   free < RESERVE_MB  -> kill the largest unprotected process
 *
 * Protected by default: init, memwatch, sh, dropbear, wpa_supplicant.
 * These have to survive for anyone to see the state and act on it.
 *
 * Two layers:
 *   1) We step in first, at 32MB free.
 *   2) If it still reaches the kernel's OOM killer, the kernel knows
 *      nothing of our list - so we write each process's
 *      /proc/<pid>/oom_score_adj to tell it:
 *        -1000  never kill this (protected)
 *          500  kill this first (everything else)
 *      The value is inherited across fork, so the children dropbear
 *      spawns per connection are protected automatically.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define RESERVE_MB    32      /* the reserve. Below this we start killing */
#define WARN_MB       64      /* start warning here */
#define POLL_MS      1000     /* how often we look, normally */
#define POLL_BUSY_MS  200     /* how often we look under pressure */
#define MAX_PROCS     256
#define TERM_GRACE_MS 500     /* grace between SIGTERM and SIGKILL */

/* /proc/<pid>/oom_score_adj values.
 * At -1000 the kernel's OOM killer drops the process from its candidates. */
#define OOM_PROTECT   (-1000)
#define OOM_SACRIFICE    500

typedef struct {
    pid_t pid;
    long  rss_kb;
    char  name[32];
} proc_t;

/* Never killed. Without these you can neither see nor fix anything. */
static const char *PROTECTED[] = {
    "init", "memwatch", "sh", "dropbear", "wpa_supplicant", NULL
};

static bool is_protected(const char *name, pid_t pid, pid_t self)
{
    if (pid == 1 || pid == self)
        return true;
    for (int i = 0; PROTECTED[i]; i++)
        if (strcmp(name, PROTECTED[i]) == 0)
            return true;
    return false;
}

/* Tell the kernel how this process should be ranked.
 * Skip the write when it already holds the value we want, so we are not
 * writing every second for nothing. */
static void set_oom_score(pid_t pid, int score)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)pid);

    char cur[32];
    if (proc_read(path, cur, sizeof(cur)) > 0) {
        long v = strtol(cur, NULL, 10);
        if (v == score)
            return;                 /* already right */
    }

    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0)
        return;                     /* the process exited meanwhile */

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", score);
    lp_write((int)fd, buf, (size_t)n);
    lp_close((int)fd);
}

/* Read the process name from /proc/<pid>/comm, without the newline. */
static bool read_comm(pid_t pid, char *out, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    char buf[64];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return false;

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, size);
    return true;
}

/* The second field of /proc/<pid>/statm is the resident page count.
 *   size resident shared text lib data dt   (all in pages) */
static long read_rss_kb(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);

    char buf[128];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return -1;

    char *p = buf;
    while (*p && *p != ' ') p++;      /* skip the first field */
    while (*p == ' ') p++;

    long pages = strtol(p, NULL, 10);
    if (pages <= 0)
        return -1;

    return pages * 4;                  /* 4KB pages -> KB */
}

/* Walk /proc and build the process list. Returns the count. */
static int scan_processes(proc_t *list, int max, pid_t self)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    static char buf[8192];
    int n = 0;

    for (;;) {
        long got = sys_getdents(fd, buf, sizeof(buf));
        if (got <= 0)
            break;

        for (long off = 0; off < got && n < max; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + 16);
            char *name = rec + 19;
            if (len == 0) break;
            off += len;

            /* An all-digit name is a process directory. */
            if (name[0] < '1' || name[0] > '9')
                continue;
            bool numeric = true;
            for (char *c = name; *c; c++)
                if (*c < '0' || *c > '9') { numeric = false; break; }
            if (!numeric)
                continue;

            pid_t pid = (pid_t)strtol(name, NULL, 10);
            long rss = read_rss_kb(pid);
            if (rss < 0)
                continue;               /* the process exited meanwhile */

            list[n].pid    = pid;
            list[n].rss_kb = rss;
            if (!read_comm(pid, list[n].name, sizeof(list[n].name)))
                strlcpy(list[n].name, "?", sizeof(list[n].name));
            (void)self;
            n++;
        }
    }

    lp_close((int)fd);
    return n;
}

/* Apply the kernel priorities to the processes we scanned.
 *
 * New processes keep appearing - dropbear forks per connection - so
 * setting this once is not enough. We revisit it periodically. */
static void apply_oom_policy(const proc_t *list, int n, pid_t self)
{
    for (int i = 0; i < n; i++) {
        bool prot = is_protected(list[i].name, list[i].pid, self);
        set_oom_score(list[i].pid, prot ? OOM_PROTECT : OOM_SACRIFICE);
    }
}

/* Kill the largest unprotected process.
 * Returns true if we killed something. */
static bool kill_largest(pid_t self, long need_kb)
{
    static proc_t list[MAX_PROCS];
    int n = scan_processes(list, MAX_PROCS, self);

    int  best = -1;
    long best_rss = 0;

    for (int i = 0; i < n; i++) {
        if (is_protected(list[i].name, list[i].pid, self))
            continue;
        if (list[i].rss_kb > best_rss) {
            best_rss = list[i].rss_kb;
            best = i;
        }
    }

    if (best < 0) {
        dprintf(STDERR_FILENO,
                "memwatch: nothing left to reclaim."
                " Only protected processes remain (%ldKB short)\n", need_kb);
        return false;
    }

    dprintf(STDERR_FILENO,
            "memwatch: out of memory - killing %s (pid %d, %ldKB)\n",
            list[best].name, (int)list[best].pid, list[best].rss_kb);

    /* Give it a chance to clean up first. */
    lp_kill(list[best].pid, SIGTERM);
    lp_sleep_ms(TERM_GRACE_MS);

    /* Still there? Force it. kill(pid, 0) only tests for existence. */
    if (lp_kill(list[best].pid, 0) == 0) {
        lp_kill(list[best].pid, SIGKILL);
        dprintf(STDERR_FILENO, "memwatch:   no response, killed it\n");
    }
    return true;
}

int main(int argc, char **argv)
{
    long reserve_kb = RESERVE_MB * 1024;
    long warn_kb    = WARN_MB * 1024;
    bool daemonize  = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            reserve_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            warn_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else {
            printf("usage: memwatch [-d] [-r reserveMB] [-w warnMB]\n");
            printf("  -d  run in the background\n");
            printf("  -r  the reserve to keep free (default %d MB)\n", RESERVE_MB);
            printf("  -w  where warnings start (default %d MB)\n", WARN_MB);
            return 2;
        }
    }

    if (warn_kb < reserve_kb)
        warn_kb = reserve_kb;

    if (daemonize) {
        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "memwatch: fork failed\n");
            return 1;
        }
        if (pid > 0)
            return 0;               /* the parent leaves at once */
        lp_setsid();
    }

    char mem[4096];
    long total = -1;
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0)
        total = proc_find_kv(mem, "MemTotal");

    if (total < 0) {
        dprintf(STDERR_FILENO,
                "memwatch: cannot read /proc/meminfo"
                " (is /proc mounted?)\n");
        return 1;
    }

    printf("memwatch: %ldMB total, reserve %ldMB, warning at %ldMB\n",
           total / 1024, reserve_kb / 1024, warn_kb / 1024);

    pid_t self = lp_getpid();
    bool  warned = false;
    int   tick = 0;

    /* Apply once immediately. This is what keeps SSH and the shell alive
     * if the kernel's OOM killer runs before memwatch gets a chance. */
    {
        static proc_t boot_list[MAX_PROCS];
        int n = scan_processes(boot_list, MAX_PROCS, self);
        apply_oom_policy(boot_list, n, self);
        printf("memwatch: kernel priorities applied to %d processes\n", n);
        printf("memwatch:   protected = init, memwatch, sh, dropbear, wpa_supplicant\n");
    }

    for (;;) {
        long avail = -1, swap_total = -1, swap_free = -1;

        if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
            avail      = proc_find_kv(mem, "MemAvailable");
            swap_total = proc_find_kv(mem, "SwapTotal");
            swap_free  = proc_find_kv(mem, "SwapFree");
        }

        if (avail < 0) {
            lp_sleep_ms(POLL_MS);
            continue;
        }

        /* Every 5s, apply the priorities to newly created processes too:
         * dropbear forks per connection, the shell forks per command. */
        if (++tick >= 5) {
            tick = 0;
            static proc_t all[MAX_PROCS];
            int n = scan_processes(all, MAX_PROCS, self);
            apply_oom_policy(all, n, self);
        }

        if (avail < reserve_kb) {
            long swap_used = (swap_total > 0 && swap_free >= 0)
                             ? swap_total - swap_free : 0;
            dprintf(STDERR_FILENO,
                    "\nmemwatch: ** memory limit - %ldMB free (reserve %ldMB)"
                    "%s\n", avail / 1024, reserve_kb / 1024,
                    swap_used > 0 ? ", swap in use" : "");

            /* Reclaim one at a time and look again. Killing several at once
             * would take more than necessary. */
            kill_largest(self, reserve_kb - avail);
            warned = true;
            lp_sleep_ms(POLL_BUSY_MS);
            continue;
        }

        if (avail < warn_kb) {
            if (!warned) {
                dprintf(STDERR_FILENO,
                        "memwatch: warning - %ldMB free\n", avail / 1024);
                warned = true;
            }
            lp_sleep_ms(POLL_BUSY_MS);
            continue;
        }

        /* Once it recovers, re-arm the warning. */
        if (warned) {
            dprintf(STDERR_FILENO,
                    "memwatch: recovered - %ldMB free\n", avail / 1024);
            warned = false;
        }
        lp_sleep_ms(POLL_MS);
    }
}
