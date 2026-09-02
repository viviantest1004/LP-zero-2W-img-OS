/* guard - the safety net.
 *
 * One daemon watching the five ways a board this small actually falls
 * over: it runs out of memory, it overheats, it browns out, one runaway
 * process takes the CPU and nothing else gets a turn, or the disk fills
 * up. Each of these is a policy the kernel does not have on its own.
 *
 *   memory   Linux has an OOM killer, but it acts only once an
 *            allocation has already failed - the machine has been
 *            crawling for a while by then - and it picks the victim
 *            itself, which may well be the SSH server. So we step in
 *            first, on our own thresholds, and we choose:
 *              free < WARN_MB     -> warn
 *              free < RESERVE_MB  -> kill the largest unprotected process
 *
 *   heat     The chip throttles itself at 80C - that is the hardware
 *            protecting the hardware. What it will not do is tell anyone
 *            or stop asking for the work. We watch the temperature and
 *            hold the CPU at its lowest frequency while it is hot, so it
 *            gets a chance to come back down.
 *
 *   power    Undervoltage - a thin cable, a phone charger - is the most
 *            common cause of a corrupted SD card on this board, and it
 *            is completely silent. The GPU firmware records it; we read
 *            that and cap the frequency, which cuts the current draw
 *            that caused it in the first place.
 *
 *   CPU      A process spinning at 100% must not make the machine
 *            unreachable. After a minute of it we push it to the back of
 *            the run queue and keep the important processes at the
 *            front. Nothing is ever killed for using the CPU - that may
 *            well be the job you asked for. It only has to yield.
 *
 *   disk     A full data partition means no logs, no saved clock, no
 *            writes at all. We warn early and drop the old rotated log
 *            when it gets critical.
 *
 * Protected: init, guard, watchdog, sh, dropbear, wpa_supplicant, logd.
 * These have to survive for anyone to see the state and act on it.
 *
 * Two layers keep them out of the memory killer's way:
 *   1) We step in first, at 32MB free.
 *   2) If it still reaches the kernel's OOM killer, the kernel knows
 *      nothing of our list - so we write each process's
 *      /proc/<pid>/oom_score_adj to tell it:
 *        -1000  never kill this (protected)
 *          500  kill this first (everything else)
 *      The value is inherited across fork, so the children dropbear
 *      spawns per connection are protected automatically.
 *
 * Everything here is best-effort by design: on a virtual machine there
 * is no temperature sensor, no firmware word and no cpufreq, and every
 * one of those reads simply fails and is skipped.
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

/* The slow pass. Everything except memory is checked on this cadence.
 * A temperature does not change in a second, and walking /proc every
 * second for nothing is exactly the idle cost we are trying not to pay. */
#define SLOW_MS      5000

/* ── Heat ──────────────────────────────────────────────────────────
 * No fan, no heatsink. Raspberry Pi's firmware starts throttling at 80C
 * and clamps hard at 85C; 70C is where we start saying something. We
 * only let go again below COOL_C - the gap is what stops it flapping
 * between the two governors every few seconds. */
#define TEMP_WARN_C     70
#define TEMP_HOT_C      80
#define TEMP_COOL_C     65

/* ── CPU ───────────────────────────────────────────────────────────
 * A process is a hog once it has held most of a core for this long.
 * /proc always counts in 100ths of a second regardless of the kernel's
 * own tick rate, so this figure is fixed. */
#define TICKS_PER_SEC   100
#define HOG_PERCENT      90
#define HOG_SECONDS      60
#define NICE_PROTECT    (-5)
#define NICE_WATCHDOG  (-10)
#define NICE_HOG         10

/* ── Disk ──────────────────────────────────────────────────────────*/
#define DISK_WARN_MB    32
#define DISK_CRIT_MB     8
#define RAMFS_WARN_MB   32    /* the root filesystem, which is memory */
#define DISK_EVERY       6    /* slow passes between disk checks (30s) */
#define OLD_LOG      "/data/log/messages.1"

/* ── Durability ────────────────────────────────────────────────────
 * ext4 commits its journal every 5s by itself; this is the belt to that
 * pair of braces, and costs nothing when nothing is dirty. */
#define SYNC_EVERY      12    /* slow passes between syncs (60s) */

/* ── Boot loop ─────────────────────────────────────────────────────
 * bootcount counts boots on the data partition and /etc/rc stops
 * trusting /data/rc.local once too many in a row have failed. Staying up
 * this long is what "a boot that worked" means, so this is where we
 * clear the count. Five minutes is past everything that starts at boot
 * and well past the watchdog's patience. */
#define BOOT_OK_PASSES  60    /* slow passes before the boot counts (5min) */
#define BOOT_COUNT_FILE "/data/boot_count"

/* /proc/<pid>/oom_score_adj values.
 * At -1000 the kernel's OOM killer drops the process from its candidates. */
#define OOM_PROTECT   (-1000)
#define OOM_SACRIFICE    500

typedef struct {
    pid_t pid;
    long  rss_kb;
    u64   ticks;        /* CPU time used so far, in 100ths of a second */
    char  name[32];
} proc_t;

typedef struct {
    const char *name;
    int         nice;
} protected_t;

/* Never killed, and kept at the front of the run queue. Without these
 * you can neither see nor fix anything.
 *
 * The watchdog sits highest of all: if it does not get a turn every few
 * seconds the board resets itself, and a machine that is merely busy
 * must never reset. */
static const protected_t PROTECTED[] = {
    { "init",           NICE_PROTECT  },
    { "guard",          NICE_PROTECT  },
    { "watchdog",       NICE_WATCHDOG },
    { "sh",             NICE_PROTECT  },
    { "dropbear",       NICE_PROTECT  },
    { "wpa_supplicant", NICE_PROTECT  },
    { "logd",           NICE_PROTECT  },
    { NULL,             0             }
};

#define NOT_PROTECTED  100    /* not a valid nice value, so unambiguous */

/* The priority a process should run at, or NOT_PROTECTED. */
static int protected_nice(const char *name, pid_t pid, pid_t self)
{
    if (pid == 1 || pid == self)
        return NICE_PROTECT;
    for (int i = 0; PROTECTED[i].name; i++)
        if (strcmp(name, PROTECTED[i].name) == 0)
            return PROTECTED[i].nice;
    return NOT_PROTECTED;
}

static bool is_protected(const char *name, pid_t pid, pid_t self)
{
    return protected_nice(name, pid, self) != NOT_PROTECTED;
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

/* utime + stime out of /proc/<pid>/stat, in 100ths of a second.
 *
 * The fields are positional, but field 2 is the process name in
 * parentheses and a name is allowed to contain ')' itself - so we find
 * the LAST one and count from there. What follows the paren is field 3,
 * and utime is field 14, so eleven fields get skipped. */
static u64 read_cpu_ticks(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    char buf[512];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return 0;

    char *p = strrchr(buf, ')');
    if (!p)
        return 0;
    p++;

    for (int field = 0; field < 11; field++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        if (!*p)
            return 0;
    }

    while (*p == ' ') p++;
    u64 utime = (u64)strtol(p, &p, 10);
    while (*p == ' ') p++;
    u64 stime = (u64)strtol(p, NULL, 10);
    return utime + stime;
}

/* ── Heat ─────────────────────────────────────────────────────────────
 * Milli-degrees C, or -1 where there is no sensor at all (which is what
 * a virtual machine looks like). */
static long read_temp_mc(void)
{
    char buf[32];
    if (proc_read("/sys/class/thermal/thermal_zone0/temp",
                  buf, sizeof(buf)) <= 0)
        return -1;
    return strtol(buf, NULL, 10);
}

/* ── Power ────────────────────────────────────────────────────────────
 * The GPU firmware keeps a word describing what it has had to do to
 * keep the board alive. It is the only place undervoltage is visible -
 * there is no voltage rail we can measure ourselves.
 *
 *   bit 0   under-voltage right now
 *   bit 1   the ARM frequency is capped right now
 *   bit 2   throttled right now
 *   bit 3   the soft temperature limit has been reached
 *   bits 16-19  the same four again, "has happened at some point"
 *
 * The sysfs name has moved between kernel versions, so we look through
 * the places it has lived rather than betting on one. */
#define THR_UNDERVOLT   0x1
#define THR_CAPPED      0x2
#define THR_THROTTLED   0x4
#define THR_SOFT_TEMP   0x8

static const char *THROTTLE_PATHS[] = {
    "/sys/devices/platform/soc/soc:firmware/get_throttled",
    "/sys/devices/platform/soc:firmware/get_throttled",
    "/sys/firmware/raspberrypi/get_throttled",
    NULL
};

static long read_throttled(void)
{
    static const char *found    = NULL;
    static bool        searched = false;

    if (!searched) {
        searched = true;
        for (int i = 0; THROTTLE_PATHS[i]; i++)
            if (lp_exists(THROTTLE_PATHS[i])) {
                found = THROTTLE_PATHS[i];
                break;
            }
    }
    if (!found)
        return -1;

    char buf[32];
    if (proc_read(found, buf, sizeof(buf)) <= 0)
        return -1;
    return strtol(buf, NULL, 16);       /* printed as 0x... */
}

/* ── The frequency lever ──────────────────────────────────────────────
 * The one response to heat and to undervoltage that costs nobody their
 * work: the job still finishes, it just takes longer. Killing a process
 * because the room is warm would be a worse answer.
 *
 * The four cores share one policy on this chip, so writing cpu0 moves
 * all of them; the rest of the writes are harmless. On a virtual machine
 * there is no cpufreq directory and every write fails, which is fine. */
static char gov_normal[32] = "ondemand";

static void read_normal_governor(void)
{
    char buf[32];
    if (proc_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                  buf, sizeof(buf)) <= 0)
        return;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (buf[0] && strcmp(buf, "powersave") != 0)
        strlcpy(gov_normal, buf, sizeof(gov_normal));
}

static void set_governor(const char *gov)
{
    for (int cpu = 0; cpu < 4; cpu++) {
        char path[96];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        long fd = lp_open(path, O_WRONLY, 0);
        if (fd < 0)
            continue;
        lp_write((int)fd, gov, strlen(gov));
        lp_close((int)fd);
    }
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
            list[n].ticks  = read_cpu_ticks(pid);
            if (!read_comm(pid, list[n].name, sizeof(list[n].name)))
                strlcpy(list[n].name, "?", sizeof(list[n].name));
            (void)self;
            n++;
        }
    }

    lp_close((int)fd);
    return n;
}

/* Apply our policy to the processes we scanned: who the kernel may kill
 * when memory runs out, and who gets the CPU first when it runs short.
 *
 * New processes keep appearing - dropbear forks one per connection, the
 * shell one per command - so setting this once is not enough. We revisit
 * it on every slow pass.
 *
 * Only the protected set has its priority forced. Everything else is
 * left where it is: a nice value someone set deliberately is a decision,
 * and overwriting it every five seconds would be rude. The one exception
 * is a hog, handled separately below. */
static void apply_priorities(const proc_t *list, int n, pid_t self)
{
    for (int i = 0; i < n; i++) {
        int want = protected_nice(list[i].name, list[i].pid, self);
        bool prot = (want != NOT_PROTECTED);

        set_oom_score(list[i].pid, prot ? OOM_PROTECT : OOM_SACRIFICE);

        if (prot && lp_getpriority(list[i].pid) != want)
            lp_setpriority(list[i].pid, want);
    }
}

/* ── CPU hogs ─────────────────────────────────────────────────────────
 * One entry per process we are keeping an eye on. The table is a fixed
 * array rather than a list because there is no memory to spare and a
 * linear walk of 256 entries every five seconds is nothing. */
typedef struct {
    pid_t pid;
    u64   ticks;       /* the reading from the previous pass */
    int   secs_hot;    /* how long it has been over the line */
    int   secs_calm;   /* how long a demoted one has behaved itself */
    int   orig_nice;   /* what it was running at before we touched it */
    bool  demoted;
    bool  seen;
} cpu_state_t;

static cpu_state_t cpu_state[MAX_PROCS];

static cpu_state_t *cpu_slot(pid_t pid)
{
    cpu_state_t *free_slot = NULL;

    for (int i = 0; i < MAX_PROCS; i++) {
        if (cpu_state[i].pid == pid)
            return &cpu_state[i];
        if (!free_slot && cpu_state[i].pid == 0)
            free_slot = &cpu_state[i];
    }
    if (!free_slot)
        return NULL;               /* full - this process waits its turn */

    free_slot->pid       = pid;
    free_slot->ticks     = 0;
    free_slot->secs_hot  = 0;
    free_slot->secs_calm = 0;
    free_slot->orig_nice = 0;
    free_slot->demoted   = false;
    return free_slot;
}

/* Find the processes holding a core to themselves and, after a minute of
 * it, move them to the back of the queue.
 *
 * A minute is deliberate. Anything shorter and an ordinary build or a
 * Python script that happens to be busy gets punished for doing its job;
 * what we are actually defending against is the process that never
 * stops. And the punishment is only a nice value - the work still runs,
 * it just no longer decides whether SSH answers. */
static void check_cpu_hogs(const proc_t *list, int n, pid_t self,
                           long elapsed_ms)
{
    if (elapsed_ms <= 0)
        return;

    for (int i = 0; i < MAX_PROCS; i++)
        cpu_state[i].seen = false;

    for (int i = 0; i < n; i++) {
        cpu_state_t *st = cpu_slot(list[i].pid);
        if (!st)
            continue;
        st->seen = true;

        u64  prev  = st->ticks;
        u64  now   = list[i].ticks;
        st->ticks  = now;

        /* First sighting, or the counter went backwards because the pid
         * was reused. Either way there is nothing to compare against. */
        if (prev == 0 || now < prev) {
            st->secs_hot = 0;
            continue;
        }

        /* Percent of one core: ticks used, over ticks available. */
        long used_ticks = (long)(now - prev);
        long have_ticks = elapsed_ms * TICKS_PER_SEC / 1000;
        long percent    = have_ticks > 0 ? used_ticks * 100 / have_ticks : 0;

        if (percent < HOG_PERCENT) {
            st->secs_hot = 0;

            /* It has stopped. Give it back what it was running at - the
             * demotion was for what it was doing, not for what it is.
             * The same minute has to pass in the other direction, so a
             * process that pauses for a moment does not get let off. */
            if (st->demoted) {
                st->secs_calm += (int)(elapsed_ms / 1000);
                if (st->secs_calm >= HOG_SECONDS) {
                    lp_setpriority(list[i].pid, st->orig_nice);
                    st->demoted   = false;
                    st->secs_calm = 0;
                    dprintf(STDERR_FILENO,
                            "guard: %s (pid %d) has settled down"
                            " - priority restored\n",
                            list[i].name, (int)list[i].pid);
                }
            }
            continue;
        }
        st->secs_calm = 0;

        /* A protected process pinning a core is worth saying out loud,
         * but it does not get demoted - that is the whole point of it
         * being protected. */
        if (is_protected(list[i].name, list[i].pid, self))
            continue;

        st->secs_hot += (int)(elapsed_ms / 1000);
        if (st->secs_hot < HOG_SECONDS || st->demoted)
            continue;

        /* Someone who already asked to run at a low priority does not
         * need us to arrange it, and moving them to 10 would be a
         * promotion. */
        int was = lp_getpriority(list[i].pid);
        if (was >= NICE_HOG)
            continue;

        if (lp_setpriority(list[i].pid, NICE_HOG) == 0) {
            st->orig_nice = was;
            st->demoted   = true;
            dprintf(STDERR_FILENO,
                    "guard: %s (pid %d) has held a core for %ds"
                    " - moved to the back of the queue\n",
                    list[i].name, (int)list[i].pid, st->secs_hot);
        }
    }

    /* Forget the processes that have gone, so their slots come free and
     * a reused pid does not inherit someone else's history. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (cpu_state[i].pid != 0 && !cpu_state[i].seen)
            cpu_state[i].pid = 0;
}

/* ── Heat and power ───────────────────────────────────────────────────
 * Both end at the same lever, so they share one piece of state: while
 * either of them says the board is in trouble, the CPU is held at its
 * lowest frequency. It is let go again only once the temperature is
 * properly back down - not merely below the line it crossed. */
static bool capped = false;

static void check_heat_and_power(void)
{
    long temp_mc  = read_temp_mc();
    long thr      = read_throttled();
    long temp_c   = temp_mc >= 0 ? temp_mc / 1000 : -1;

    bool hot   = (temp_c >= 0 && temp_c >= TEMP_HOT_C);
    bool cool  = (temp_c < 0  || temp_c <= TEMP_COOL_C);
    bool power = (thr > 0 && (thr & (THR_UNDERVOLT | THR_CAPPED)) != 0);

    static bool warned_temp = false;
    static bool warned_power = false;

    if (temp_c >= TEMP_WARN_C && !warned_temp) {
        dprintf(STDERR_FILENO, "guard: %ldC - getting warm\n", temp_c);
        warned_temp = true;
    } else if (temp_c >= 0 && temp_c < TEMP_WARN_C) {
        warned_temp = false;
    }

    if (power && !warned_power) {
        dprintf(STDERR_FILENO,
                "guard: ** the power supply is not keeping up"
                " (firmware says 0x%x)\n"
                "guard:    this is what corrupts SD cards."
                " Use a better cable or charger.\n", (unsigned)thr);
        warned_power = true;
    } else if (!power) {
        warned_power = false;
    }

    if ((hot || power) && !capped) {
        set_governor("powersave");
        capped = true;
        dprintf(STDERR_FILENO,
                "guard: holding the CPU at its lowest speed (%s)\n",
                hot ? "too hot" : "power");
    } else if (capped && cool && !power) {
        set_governor(gov_normal);
        capped = false;
        dprintf(STDERR_FILENO, "guard: back to normal speed");
        if (temp_c >= 0)
            dprintf(STDERR_FILENO, " (%ldC)", temp_c);
        dprintf(STDERR_FILENO, "\n");
    }
}

/* Mark this boot as one that worked. See BOOT_OK_PASSES above. */
static void clear_boot_count(void)
{
    long fd = lp_open(BOOT_COUNT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;                     /* no data partition - nothing counted */
    lp_write((int)fd, "0\n", 2);
    lp_close((int)fd);
}

/* ── Disk ─────────────────────────────────────────────────────────────
 * A full data partition is quiet until everything stops at once: the log
 * cannot be written, the clock cannot be saved, and a new SSH session
 * cannot write its own history. The one thing we can free without asking
 * is the previous rotated log - it has already been superseded. */
static void check_disk(void)
{
    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) < 0 || totalb == 0)
        return;                     /* no data partition - all in RAM */

    long free_mb = (long)(freeb / (1024 * 1024));
    static bool warned = false;

    if (free_mb <= DISK_CRIT_MB) {
        dprintf(STDERR_FILENO,
                "guard: ** /data has %ldMB left\n", free_mb);
        if (lp_exists(OLD_LOG) && lp_unlink(OLD_LOG) == 0)
            dprintf(STDERR_FILENO,
                    "guard:    dropped the old log (%s)\n", OLD_LOG);
        warned = true;
    } else if (free_mb <= DISK_WARN_MB) {
        if (!warned) {
            dprintf(STDERR_FILENO,
                    "guard: /data is filling up - %ldMB left\n", free_mb);
            warned = true;
        }
    } else {
        warned = false;
    }

    /* The root filesystem is RAM. Space used there is memory that no
     * process owns, so the memory killer cannot win it back: it would
     * kill process after process while the pages stay exactly where they
     * are. Worth saying plainly, because nothing about the free memory
     * figure points at a file as the cause. */
    static bool warned_root = false;
    if (lp_fs_space("/", &freeb, &totalb) == 0 && totalb > 0) {
        long root_free_mb = (long)(freeb / (1024 * 1024));
        if (root_free_mb <= RAMFS_WARN_MB && !warned_root) {
            dprintf(STDERR_FILENO,
                    "guard: ** the root filesystem is RAM and has %ldMB"
                    " left\n"
                    "guard:    files outside /data and /tmp are held in"
                    " memory. Delete some.\n", root_free_mb);
            warned_root = true;
        } else if (root_free_mb > RAMFS_WARN_MB) {
            warned_root = false;
        }
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
                "guard: nothing left to reclaim."
                " Only protected processes remain (%ldKB short)\n", need_kb);
        return false;
    }

    dprintf(STDERR_FILENO,
            "guard: out of memory - killing %s (pid %d, %ldKB)\n",
            list[best].name, (int)list[best].pid, list[best].rss_kb);

    /* Give it a chance to clean up first. */
    lp_kill(list[best].pid, SIGTERM);
    lp_sleep_ms(TERM_GRACE_MS);

    /* Still there? Force it. kill(pid, 0) only tests for existence. */
    if (lp_kill(list[best].pid, 0) == 0) {
        lp_kill(list[best].pid, SIGKILL);
        dprintf(STDERR_FILENO, "guard:   no response, killed it\n");
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
            printf("usage: guard [-d] [-r reserveMB] [-w warnMB]\n");
            printf("  -d  run in the background\n");
            printf("  -r  the memory reserve to keep free (default %d MB)\n",
                   RESERVE_MB);
            printf("  -w  where memory warnings start (default %d MB)\n",
                   WARN_MB);
            printf("\nWatches memory, temperature, the power supply, CPU"
                   " hogs and disk space.\n");
            return 2;
        }
    }

    if (warn_kb < reserve_kb)
        warn_kb = reserve_kb;

    if (daemonize) {
        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "guard: fork failed\n");
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
                "guard: cannot read /proc/meminfo (is /proc mounted?)\n");
        return 1;
    }

    /* Remember what the CPU was set to before we touch it, so that
     * whatever the board came up with is what it goes back to. */
    read_normal_governor();

    printf("guard: %ldMB total, reserve %ldMB, warning at %ldMB\n",
           total / 1024, reserve_kb / 1024, warn_kb / 1024);

    {
        long t = read_temp_mc();
        if (t >= 0)
            printf("guard: %ldC now, slowing down at %dC\n",
                   t / 1000, TEMP_HOT_C);
        else
            printf("guard: no temperature sensor here\n");
    }

    pid_t self = lp_getpid();
    bool  warned = false;

    /* Apply once immediately. This is what keeps SSH and the shell alive
     * if the kernel's OOM killer runs before guard gets a chance. */
    {
        static proc_t boot_list[MAX_PROCS];
        int n = scan_processes(boot_list, MAX_PROCS, self);
        apply_priorities(boot_list, n, self);
        printf("guard: priorities applied to %d processes\n", n);
        printf("guard:   protected = init, guard, watchdog, sh, dropbear,"
               " wpa_supplicant, logd\n");
    }

    /* Time is counted in the sleeps we ourselves ask for, not from the
     * clock. The clock jumps the moment ntp gets an answer, and a jump
     * would turn one CPU sample into nonsense. */
    long ms_since_slow = 0;
    int  slow_passes   = 0;

    for (;;) {
        long avail = -1, swap_total = -1, swap_free = -1;
        long sleep_ms = POLL_MS;

        /* ── the slow pass ── */
        if (ms_since_slow >= SLOW_MS) {
            static proc_t all[MAX_PROCS];
            int n = scan_processes(all, MAX_PROCS, self);

            apply_priorities(all, n, self);
            check_cpu_hogs(all, n, self, ms_since_slow);
            check_heat_and_power();

            slow_passes++;
            if (slow_passes % DISK_EVERY == 0)
                check_disk();
            if (slow_passes % SYNC_EVERY == 0)
                lp_sync();
            if (slow_passes == BOOT_OK_PASSES)
                clear_boot_count();

            ms_since_slow = 0;
        }

        /* ── memory ── */
        if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
            avail      = proc_find_kv(mem, "MemAvailable");
            swap_total = proc_find_kv(mem, "SwapTotal");
            swap_free  = proc_find_kv(mem, "SwapFree");
        }

        if (avail < 0) {
            /* nothing to go on this time round */
        } else if (avail < reserve_kb) {
            long swap_used = (swap_total > 0 && swap_free >= 0)
                             ? swap_total - swap_free : 0;
            dprintf(STDERR_FILENO,
                    "\nguard: ** memory limit - %ldMB free (reserve %ldMB)"
                    "%s\n", avail / 1024, reserve_kb / 1024,
                    swap_used > 0 ? ", swap in use" : "");

            /* Reclaim one at a time and look again. Killing several at
             * once would take more than necessary. */
            kill_largest(self, reserve_kb - avail);
            warned = true;
            sleep_ms = POLL_BUSY_MS;
        } else if (avail < warn_kb) {
            if (!warned) {
                dprintf(STDERR_FILENO,
                        "guard: warning - %ldMB free\n", avail / 1024);
                warned = true;
            }
            sleep_ms = POLL_BUSY_MS;
        } else if (warned) {
            /* Once it recovers, re-arm the warning. */
            dprintf(STDERR_FILENO,
                    "guard: recovered - %ldMB free\n", avail / 1024);
            warned = false;
        }

        lp_sleep_ms(sleep_ms);
        ms_since_slow += sleep_ms;
    }
}
