/* tasks - the system monitor: what is running, and what it is costing.
 *
 *   lp-tasks                 open on the page used last
 *   lp-tasks --page=cpu      apps, processes, cpu, memory, gpu, drives,
 *                            network, battery
 *
 * The pages are the spec's (apps-and-settings §2-2) and the layout is the
 * owner's mockup (design/reference/task-manager-settings-mockup.html): a
 * sidebar of Apps and Processes, then the resources; each resource a
 * 60-second graph with its number written large on it, cards of the
 * figures that go with it, and a sentence where a number alone would not
 * say whether it is good or bad (a disk under 15% free is amber and says
 * so; a battery says how long it will last at this rate).
 *
 * ── What it costs to watch ──
 *
 * A system monitor that shows up in its own CPU graph is a joke nobody
 * laughs at twice, and this machine's owner asked for heavy optimisation.
 * So the work is split by what is on screen:
 *
 *  - Once a second, always: /proc/stat, /proc/meminfo, /proc/diskstats,
 *    /proc/net/dev, one hwmon file, the battery. A few small reads; the
 *    history behind every graph has to keep coming even while its page
 *    is not shown, or switching to it would show a blank minute.
 *  - Once a second, only while Apps, Processes or Memory is showing and
 *    the window is not minimised: the walk over /proc/<pid>. That is the
 *    expensive part (hundreds of small files), and nobody reads a
 *    process list through a minimised window.
 *  - Every frame, only while a graph is mapped and its window is not
 *    minimised: the graph's scroll. The tick callback is added on map and
 *    removed on unmap and on minimise, so a window left open on another
 *    workspace costs one timer a second and nothing per frame.
 *
 * ── How a graph moves ──
 *
 * The samples come once a second; the graph glides between them. It is
 * drawn one sample late on purpose: the newest point enters at the right
 * edge the moment it is known, and the whole line travels left at one
 * sample-width per second, so there is never a jump and never a gap to
 * fill. The line itself is a cairo drawing made ONCE per sample and kept
 * as a render node; each frame only translates that node under a clip.
 * With the GL renderer a node that has not changed is a cached texture,
 * so a frame of a scrolling graph is one textured quad, not a cairo
 * redraw of a 3300-pixel-wide path. The large number on the graph and
 * the per-core bars follow critically damped springs (no overshoot - a
 * CPU bar that bounces past the real value is a lie for 100ms), and they
 * are drawn by the widget, not set as label text, so easing them
 * re-lays nothing out.
 *
 * ── What it can and cannot do to a process ──
 *
 * End (SIGTERM) and Kill (SIGKILL), each after a confirmation. Only the
 * person's own processes: signalling another account's needs root, and
 * the one root helper this desktop has (lp-privd) deliberately has no
 * "kill any process" verb. The error says so when it happens.
 */
#define _GNU_SOURCE 1
#include "lp-kit.h"

#include <gio/gdesktopappinfo.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define APP_ID      "org.lpzero.Tasks"
#define STATE_NAME  "tasks"
#define WINDOW_W    1120
#define WINDOW_H    780
#define SIDEBAR_W   176

#define HIST        62          /* samples kept: 60 s on screen + margin */
#define WINDOW_S    60.0        /* seconds a graph spans */
#define BAT_HIST    360         /* 3 hours of battery at one per 30 s */
#define LOW_FREE    0.15        /* spec: amber under 15% free */

/* ═══════════════════════════════════════════════════════════════════
 * Reading /proc and /sys
 * ═══════════════════════════════════════════════════════════════════ */

/* Small files are read into a caller's buffer with one read(): these are
 * kernel-generated and never short-read at this size, and GLib's
 * allocate-and-grow reader is the wrong tool for four hundred of them a
 * second. */
static ssize_t read_small(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    return r;
}

static gint64 read_num(const char *path, gint64 fallback)
{
    char b[64];
    if (read_small(path, b, sizeof b) <= 0)
        return fallback;
    char *end = NULL;
    gint64 v = g_ascii_strtoll(b, &end, 10);
    return end == b ? fallback : v;
}

static char *read_word(const char *path)
{
    char b[256];
    if (read_small(path, b, sizeof b) <= 0)
        return NULL;
    return g_strstrip(g_strdup(b));
}

/* A history of one number: oldest first, with the monotonic time each
 * sample was taken, so the graph can place it exactly. */
typedef struct {
    double v[HIST];
    gint64 t[HIST];
    int    n;
    guint  serial;     /* bumps on every push: the graph's "rebuild" cue */
} Series;

static void series_push(Series *s, gint64 t, double v)
{
    if (s->n == HIST) {
        memmove(s->v, s->v + 1, sizeof s->v[0] * (HIST - 1));
        memmove(s->t, s->t + 1, sizeof s->t[0] * (HIST - 1));
        s->n--;
    }
    s->v[s->n] = v;
    s->t[s->n] = t;
    s->n++;
    s->serial++;
}

static double series_last(const Series *s)
{
    return s->n ? s->v[s->n - 1] : 0.0;
}

typedef struct {
    char    *name;
    char    *path;       /* mount point */
    char    *dev;
    guint64  size, avail;
} Fs;

typedef struct {
    /* CPU */
    int      ncpu;
    guint64 *prev_total, *prev_idle;  /* [0] the whole machine, [1..] cores */
    double   cpu;                     /* % of the whole machine */
    double  *core;                    /* % per core */
    guint64  delta_total;             /* all-CPU jiffies in the last second */
    Series   cpu_h;
    char    *model;
    int      cores;                   /* physical */
    double   mhz, max_mhz, base_mhz;
    char    *l3;
    gboolean virt;
    double   temp;                    /* NAN when there is no sensor */
    int      nprocs, nthreads;
    double   uptime;

    /* Memory, bytes */
    guint64  mem_total, mem_avail, mem_free, mem_cache, swap_total, swap_free;
    Series   mem_h;

    /* Drives, bytes/s */
    guint64  prev_rd, prev_wr;
    double   rd, wr;
    Series   rd_h, wr_h;
    GPtrArray *fs;                    /* Fs*, refreshed while shown */

    /* Network, bytes/s */
    guint64  prev_rx, prev_tx, rx_total, tx_total;
    double   rx, tx;
    Series   rx_h, tx_h;

    /* GPU (i915: busy from rc6 residency) */
    gboolean gpu_ok;
    gint64   prev_rc6, prev_rc6_t;
    double   gpu;
    int      gpu_mhz, gpu_max;
    char    *gpu_driver;
    char    *dgpu;                    /* discrete GPU state, or NULL */
    Series   gpu_h;

    /* Battery */
    gboolean bat;
    int      bat_pct;
    char    *bat_state;
    double   bat_w;
    int      bat_min;                 /* -1 unknown */
    double   bat_health;              /* -1 unknown */
    int      bat_cycles;              /* -1 unknown */
    char    *bat_path;
    double   bat_hist[BAT_HIST];
    int      bat_n;
    gint64   bat_last;

    gboolean primed;
    gint64   now;
} Sys;

static Sys S;

static void read_cpuinfo(void)
{
    char *text = NULL;
    S.ncpu = 0;
    if (g_file_get_contents("/proc/cpuinfo", &text, NULL, NULL)) {
        GHashTable *phys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char *pid = NULL;
        for (char *l = text, *n; l && *l; l = n) {
            n = strchr(l, '\n');
            if (n)
                *n++ = '\0';
            char *colon = strchr(l, ':');
            if (!colon)
                continue;
            char *val = g_strstrip(colon + 1);
            if (g_str_has_prefix(l, "processor"))
                S.ncpu++;
            else if (g_str_has_prefix(l, "model name") && !S.model)
                S.model = g_strdup(val);
            else if (g_str_has_prefix(l, "physical id")) {
                g_free(pid);
                pid = g_strdup(val);
            } else if (g_str_has_prefix(l, "core id")) {
                g_hash_table_add(phys, g_strdup_printf("%s/%s", pid ? pid : "0", val));
            } else if (g_str_has_prefix(l, "flags") && !S.virt) {
                S.virt = strstr(val, " vmx") || strstr(val, " svm");
            }
        }
        S.cores = (int)g_hash_table_size(phys);
        g_hash_table_unref(phys);
        g_free(pid);
        g_free(text);
    }
    if (S.ncpu <= 0)
        S.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (S.ncpu <= 0)
        S.ncpu = 1;
    if (S.cores <= 0)
        S.cores = S.ncpu;
    if (!S.model)
        S.model = g_strdup(T("Processor", "프로세서"));
    S.max_mhz = read_num("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", 0) / 1000.0;
    S.base_mhz = read_num("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency", 0) / 1000.0;
    for (int i = 0; i < 5 && !S.l3; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu0/cache/index%d/level", i);
        if (read_num(p, 0) == 3) {
            g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
            S.l3 = read_word(p);
        }
    }
    S.prev_total = g_new0(guint64, S.ncpu + 1);
    S.prev_idle = g_new0(guint64, S.ncpu + 1);
    S.core = g_new0(double, S.ncpu);
}

static void sample_cpu(void)
{
    char buf[16384];
    if (read_small("/proc/stat", buf, sizeof buf) <= 0)
        return;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        if (strncmp(l, "cpu", 3) != 0)
            break;
        int idx = 0;
        char *p = l + 3;
        if (*p != ' ') {
            idx = (int)strtol(p, &p, 10) + 1;
            if (idx > S.ncpu)
                continue;
        }
        guint64 f[10] = { 0 };
        for (int i = 0; i < 10; i++)
            f[i] = g_ascii_strtoull(p, &p, 10);
        /* user nice system idle iowait irq softirq steal (guest is
         * already inside user). iowait is idle: the CPU was free. */
        guint64 idle = f[3] + f[4];
        guint64 total = f[0] + f[1] + f[2] + f[3] + f[4] + f[5] + f[6] + f[7];
        guint64 dt = total - S.prev_total[idx], di = idle - S.prev_idle[idx];
        double pct = (S.primed && dt) ? 100.0 * (double)(dt - di) / (double)dt : 0.0;
        if (idx == 0) {
            S.cpu = pct;
            S.delta_total = dt;
        } else {
            S.core[idx - 1] = pct;
        }
        S.prev_total[idx] = total;
        S.prev_idle[idx] = idle;
    }
    /* Mean of the cores' current clocks: what "3.84 GHz" means. */
    double sum = 0;
    int got = 0;
    for (int i = 0; i < S.ncpu; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        gint64 k = read_num(p, -1);
        if (k > 0) {
            sum += k / 1000.0;
            got++;
        }
    }
    S.mhz = got ? sum / got : 0;
    char up[64];
    if (read_small("/proc/uptime", up, sizeof up) > 0)
        S.uptime = g_ascii_strtod(up, NULL);
}

static void sample_mem(void)
{
    char buf[4096];
    if (read_small("/proc/meminfo", buf, sizeof buf) <= 0)
        return;
    guint64 buffers = 0, cached = 0, srecl = 0, shmem = 0;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char *c = strchr(l, ':');
        if (!c)
            continue;
        *c = '\0';
        guint64 kib = g_ascii_strtoull(c + 1, NULL, 10) * 1024;
        if (!strcmp(l, "MemTotal")) S.mem_total = kib;
        else if (!strcmp(l, "MemFree")) S.mem_free = kib;
        else if (!strcmp(l, "MemAvailable")) S.mem_avail = kib;
        else if (!strcmp(l, "Buffers")) buffers = kib;
        else if (!strcmp(l, "Cached")) cached = kib;
        else if (!strcmp(l, "SReclaimable")) srecl = kib;
        else if (!strcmp(l, "Shmem")) shmem = kib;
        else if (!strcmp(l, "SwapTotal")) S.swap_total = kib;
        else if (!strcmp(l, "SwapFree")) S.swap_free = kib;
    }
    guint64 cache = buffers + cached + srecl;
    S.mem_cache = cache > shmem ? cache - shmem : 0;
}

/* Whole disks only: a partition's sectors are already in its disk's. */
static gboolean whole_disk(const char *name)
{
    if (g_str_has_prefix(name, "loop") || g_str_has_prefix(name, "ram") ||
        g_str_has_prefix(name, "zram") || g_str_has_prefix(name, "dm-") ||
        g_str_has_prefix(name, "sr"))
        return FALSE;
    char p[128];
    g_snprintf(p, sizeof p, "/sys/block/%s", name);
    return g_file_test(p, G_FILE_TEST_IS_DIR);
}

static void sample_disk(double secs)
{
    char buf[16384];
    if (read_small("/proc/diskstats", buf, sizeof buf) <= 0)
        return;
    guint64 rd = 0, wr = 0;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        unsigned maj, min;
        char name[64];
        unsigned long long f[11];
        if (sscanf(l, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu",
                   &maj, &min, name, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                   &f[6], &f[7]) < 10)
            continue;
        if (!whole_disk(name))
            continue;
        rd += f[2] * 512;
        wr += f[6] * 512;
    }
    if (S.primed && secs > 0) {
        S.rd = (rd - S.prev_rd) / secs;
        S.wr = (wr - S.prev_wr) / secs;
    }
    S.prev_rd = rd;
    S.prev_wr = wr;
}

static void sample_net(double secs)
{
    char buf[8192];
    if (read_small("/proc/net/dev", buf, sizeof buf) <= 0)
        return;
    guint64 rx = 0, tx = 0;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char *c = strchr(l, ':');
        if (!c)
            continue;
        *c = '\0';
        char *name = g_strstrip(l);
        if (!strcmp(name, "lo"))
            continue;
        unsigned long long f[9];
        if (sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8]) < 9)
            continue;
        rx += f[0];
        tx += f[8];
    }
    if (S.primed && secs > 0) {
        S.rx = (rx - S.prev_rx) / secs;
        S.tx = (tx - S.prev_tx) / secs;
    }
    S.prev_rx = rx;
    S.prev_tx = tx;
    S.rx_total = rx;
    S.tx_total = tx;
}

/* The hottest package or core sensor; the same search `temp` makes,
 * hwmon numbered with gaps, so it does not stop at the first missing. */
static char *temp_path;

static void find_temp(void)
{
    double best = -1;
    for (int i = 0; i < 32; i++) {
        char nm[96];
        g_snprintf(nm, sizeof nm, "/sys/class/hwmon/hwmon%d/name", i);
        char *name = read_word(nm);
        if (!name)
            continue;
        gboolean cpuish = !strcmp(name, "coretemp") || !strcmp(name, "k10temp") ||
                          !strcmp(name, "zenpower") || !strcmp(name, "cpu_thermal") ||
                          !strcmp(name, "acpitz");
        for (int k = 1; k < 16 && cpuish; k++) {
            char p[128];
            g_snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_input", i, k);
            gint64 v = read_num(p, -1);
            if (v < 0)
                continue;
            /* Prefer the package sensor (temp1 on coretemp); acpitz only
             * if nothing better turns up. */
            double score = (strcmp(name, "acpitz") ? 1000.0 : 0.0) + (k == 1 ? 100.0 : 0.0);
            if (score > best) {
                best = score;
                g_free(temp_path);
                temp_path = g_strdup(p);
            }
        }
        g_free(name);
    }
    if (!temp_path && g_file_test("/sys/class/thermal/thermal_zone0/temp", G_FILE_TEST_EXISTS))
        temp_path = g_strdup("/sys/class/thermal/thermal_zone0/temp");
}

static char *drm_card;   /* "/sys/class/drm/card0" for the Intel GPU */

static void find_gpu(void)
{
    for (int i = 0; i < 4; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/class/drm/card%d/device/driver", i);
        char *link = g_file_read_link(p, NULL);
        if (!link)
            continue;
        char *drv = g_path_get_basename(link);
        g_free(link);
        if (!drm_card) {
            drm_card = g_strdup_printf("/sys/class/drm/card%d", i);
            S.gpu_driver = g_strdup(drv);
        }
        g_free(drv);
    }
    if (drm_card) {
        char p[128];
        g_snprintf(p, sizeof p, "%s/power/rc6_residency_ms", drm_card);
        S.gpu_ok = g_file_test(p, G_FILE_TEST_EXISTS);
        g_snprintf(p, sizeof p, "%s/gt_max_freq_mhz", drm_card);
        S.gpu_max = (int)read_num(p, 0);
    }
}

/* The GTX 960M on the XPS sleeps until something asks for it (Optimus,
 * no mux): runtime PM says whether it is awake. */
static void sample_dgpu(void)
{
    g_clear_pointer(&S.dgpu, g_free);
    GDir *d = g_dir_open("/sys/bus/pci/devices", 0, NULL);
    if (!d)
        return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        char p[160];
        g_snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", e);
        char *v = read_word(p);
        g_snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", e);
        char *c = read_word(p);
        if (v && c && !strcmp(v, "0x10de") && g_str_has_prefix(c, "0x03")) {
            g_snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/power/runtime_status", e);
            char *st = read_word(p);
            S.dgpu = g_strdup(st && !strcmp(st, "suspended")
                ? T("NVIDIA GPU asleep - it wakes when an application asks for it",
                    "NVIDIA GPU 휴면 중 - 앱이 요청하면 깨어납니다")
                : T("NVIDIA GPU awake", "NVIDIA GPU 동작 중"));
            g_free(st);
        }
        g_free(v);
        g_free(c);
    }
    g_dir_close(d);
}

static void sample_gpu(void)
{
    if (!drm_card)
        return;
    char p[128];
    g_snprintf(p, sizeof p, "%s/gt_act_freq_mhz", drm_card);
    S.gpu_mhz = (int)read_num(p, 0);
    if (!S.gpu_ok)
        return;
    g_snprintf(p, sizeof p, "%s/power/rc6_residency_ms", drm_card);
    gint64 rc6 = read_num(p, -1);
    gint64 t = g_get_monotonic_time() / 1000;
    if (rc6 >= 0 && S.prev_rc6_t) {
        double idle = (double)(rc6 - S.prev_rc6) / (double)MAX(1, t - S.prev_rc6_t);
        S.gpu = CLAMP(100.0 * (1.0 - idle), 0.0, 100.0);
    }
    S.prev_rc6 = rc6;
    S.prev_rc6_t = t;
}

static void find_battery(void)
{
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d)
        return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        char p[160];
        g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e);
        char *t = read_word(p);
        if (t && !strcmp(t, "Battery") && !S.bat_path)
            S.bat_path = g_strdup_printf("/sys/class/power_supply/%s", e);
        g_free(t);
    }
    g_dir_close(d);
    S.bat = S.bat_path != NULL;
}

static gint64 bat_num(const char *f)
{
    char p[200];
    g_snprintf(p, sizeof p, "%s/%s", S.bat_path, f);
    return read_num(p, -1);
}

static void sample_battery(void)
{
    if (!S.bat)
        return;
    S.bat_pct = (int)bat_num("capacity");
    char p[200];
    g_snprintf(p, sizeof p, "%s/status", S.bat_path);
    g_free(S.bat_state);
    S.bat_state = read_word(p);
    /* Power in µW directly, or µA x µV. */
    gint64 pw = bat_num("power_now");
    if (pw < 0) {
        gint64 cu = bat_num("current_now"), vo = bat_num("voltage_now");
        pw = (cu >= 0 && vo >= 0) ? (gint64)((double)cu * (double)vo / 1e6) : -1;
    }
    S.bat_w = pw >= 0 ? pw / 1e6 : -1;
    gint64 en = bat_num("energy_now"), ef = bat_num("energy_full"),
           efd = bat_num("energy_full_design");
    if (en < 0) {
        gint64 cn = bat_num("charge_now"), vo = bat_num("voltage_min_design");
        if (cn >= 0 && vo > 0)
            en = (gint64)((double)cn * vo / 1e6);
        ef = bat_num("charge_full");
        efd = bat_num("charge_full_design");
    }
    S.bat_health = (ef > 0 && efd > 0) ? 100.0 * ef / efd : -1;
    S.bat_cycles = (int)bat_num("cycle_count");
    S.bat_min = -1;
    if (S.bat_state && !strcmp(S.bat_state, "Discharging") && pw > 0 && en > 0)
        S.bat_min = (int)(60.0 * en / pw);
    else if (S.bat_state && !strcmp(S.bat_state, "Charging") && pw > 0 && en >= 0 && ef > en)
        S.bat_min = (int)(60.0 * (ef - en) / pw);
    if (!S.bat_last || S.now - S.bat_last >= 30 * G_USEC_PER_SEC) {
        S.bat_last = S.now;
        if (S.bat_n == BAT_HIST) {
            memmove(S.bat_hist, S.bat_hist + 1, sizeof(double) * (BAT_HIST - 1));
            S.bat_n--;
        }
        S.bat_hist[S.bat_n++] = S.bat_pct;
    }
}

/* Mounted filesystems that are real disks, for the Drives page. */
static void sample_fs(void)
{
    if (!S.fs)
        S.fs = g_ptr_array_new();
    for (guint i = 0; i < S.fs->len; i++) {
        Fs *f = g_ptr_array_index(S.fs, i);
        g_free(f->name);
        g_free(f->path);
        g_free(f->dev);
        g_free(f);
    }
    g_ptr_array_set_size(S.fs, 0);
    char *text = NULL;
    if (!g_file_get_contents("/proc/self/mountinfo", &text, NULL, NULL))
        return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (char *l = text, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char **f = g_strsplit(l, " ", 0);
        int nf = (int)g_strv_length(f);
        int dash = -1;
        for (int i = 6; i < nf; i++)
            if (!strcmp(f[i], "-")) {
                dash = i;
                break;
            }
        if (dash > 0 && dash + 2 < nf && g_str_has_prefix(f[dash + 2], "/dev/") &&
            !g_str_has_prefix(f[dash + 2], "/dev/loop") &&
            !g_hash_table_contains(seen, f[dash + 2])) {
            const char *mp = f[4];
            if (!strcmp(mp, "/") || g_str_has_prefix(mp, "/home") ||
                g_str_has_prefix(mp, "/media/") || g_str_has_prefix(mp, "/mnt/") ||
                g_str_has_prefix(mp, "/boot") || !strcmp(mp, "/data")) {
                struct statvfs sv;
                char *path = g_strcompress(mp);
                if (statvfs(path, &sv) == 0 && sv.f_blocks) {
                    Fs *fs = g_new0(Fs, 1);
                    fs->path = path;
                    fs->dev = g_strdup(f[dash + 2]);
                    fs->size = (guint64)sv.f_blocks * sv.f_frsize;
                    fs->avail = (guint64)sv.f_bavail * sv.f_frsize;
                    fs->name = !strcmp(path, "/") ? g_strdup(T("System", "시스템"))
                                                  : g_path_get_basename(path);
                    g_ptr_array_add(S.fs, fs);
                    g_hash_table_add(seen, g_strdup(f[dash + 2]));
                } else {
                    g_free(path);
                }
            }
        }
        g_strfreev(f);
    }
    g_hash_table_unref(seen);
    g_free(text);
}

/* ═══════════════════════════════════════════════════════════════════
 * Processes
 * ═══════════════════════════════════════════════════════════════════ */

#define LPT_TYPE_PROC (lpt_proc_get_type())
G_DECLARE_FINAL_TYPE(LptProc, lpt_proc, LPT, PROC, GObject)

struct _LptProc {
    GObject  parent_instance;
    int      pid;
    guint    uid;
    char    *name;       /* argv[0]'s basename, or comm */
    char    *cmd;
    char    *user;
    double   cpu;        /* % of the whole machine */
    guint64  rss;
    double   io;         /* bytes/s, -1 when not readable */
    int      threads;
    guint64  prev_ticks;
    guint64  prev_io;
    gboolean io_ok;
    guint    gen;        /* the scan that last saw it */
    char    *app;        /* desktop id of the application it belongs to */
};

G_DEFINE_TYPE(LptProc, lpt_proc, G_TYPE_OBJECT)
static guint proc_changed;

static void lpt_proc_finalize(GObject *o)
{
    LptProc *p = LPT_PROC(o);
    g_free(p->name);
    g_free(p->cmd);
    g_free(p->user);
    g_free(p->app);
    G_OBJECT_CLASS(lpt_proc_parent_class)->finalize(o);
}

static void lpt_proc_class_init(LptProcClass *k)
{
    G_OBJECT_CLASS(k)->finalize = lpt_proc_finalize;
    proc_changed = g_signal_new("changed", LPT_TYPE_PROC, G_SIGNAL_RUN_LAST,
                                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lpt_proc_init(LptProc *p) { p->io = -1; }

static GHashTable *procs;       /* pid -> LptProc (a ref) */
static GListStore *proc_store;
static guint scan_gen;
static GHashTable *users;       /* uid -> name */
static GHashTable *exe_apps;    /* executable basename -> desktop id */
static long page_size;
static long clk_tck;

static const char *user_name(guint uid)
{
    const char *n = g_hash_table_lookup(users, GUINT_TO_POINTER(uid + 1));
    if (n)
        return n;
    struct passwd *pw = getpwuid(uid);
    char *s = pw ? g_strdup(pw->pw_name) : g_strdup_printf("%u", uid);
    g_hash_table_insert(users, GUINT_TO_POINTER(uid + 1), s);
    return s;
}

/* Which application each executable belongs to, from the .desktop
 * files: "Exec=firefox-esr %u" -> firefox-esr -> firefox-esr.desktop. */
static void load_exe_apps(void)
{
    exe_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        GAppInfo *ai = l->data;
        if (!g_app_info_should_show(ai) || !g_app_info_get_id(ai))
            continue;
        const char *exe = g_app_info_get_executable(ai);
        if (!exe || !*exe)
            continue;
        char *base = g_path_get_basename(exe);
        if (!g_hash_table_contains(exe_apps, base) && strcmp(base, "env") &&
            strcmp(base, "sh") && strcmp(base, "bash"))
            g_hash_table_insert(exe_apps, base, g_strdup(g_app_info_get_id(ai)));
        else
            g_free(base);
    }
    g_list_free_full(all, g_object_unref);
}

static gboolean scan_one(int pid, guint64 total_delta)
{
    char path[64], buf[2048];
    g_snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_small(path, buf, sizeof buf) <= 0)
        return FALSE;
    /* "pid (comm) state ..." - comm may hold spaces and parentheses, so
     * the fields start after the LAST ')'. */
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp)
        return FALSE;
    char *comm = g_strndup(lp + 1, (gsize)(rp - lp - 1));
    char *p = rp + 2;
    unsigned long long f[20] = { 0 };
    /* state is field 3; we want utime(14) stime(15) threads(20). */
    char state = *p;
    (void)state;
    p += 2;
    for (int i = 4; i <= 20; i++)
        f[i - 4 < 20 ? i - 4 : 19] = g_ascii_strtoull(p, &p, 10);
    guint64 ticks = f[14 - 4] + f[15 - 4];
    int threads = (int)f[20 - 4];

    LptProc *pr = g_hash_table_lookup(procs, GINT_TO_POINTER(pid));
    gboolean fresh = pr == NULL;
    if (fresh) {
        pr = g_object_new(LPT_TYPE_PROC, NULL);
        pr->pid = pid;
        g_snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        char cb[1024];
        ssize_t n = read_small(path, cb, sizeof cb);
        if (n > 0) {
            for (ssize_t i = 0; i < n - 1; i++)
                if (cb[i] == '\0')
                    cb[i] = ' ';
            pr->cmd = g_strdup(cb);
            char *first = g_strndup(cb, strcspn(cb, " "));
            pr->name = g_path_get_basename(first);
            g_free(first);
        }
        if (!pr->name || !*pr->name || strlen(pr->name) < strlen(comm) / 2) {
            g_free(pr->name);
            pr->name = g_strdup(comm);
        }
        if (!pr->cmd)
            pr->cmd = g_strdup_printf("[%s]", comm);   /* a kernel thread */
        struct stat st;
        g_snprintf(path, sizeof path, "/proc/%d", pid);
        if (stat(path, &st) == 0)
            pr->uid = st.st_uid;
        pr->user = g_strdup(user_name(pr->uid));
        const char *app = g_hash_table_lookup(exe_apps, pr->name);
        if (!app) {
            const char *app2 = g_hash_table_lookup(exe_apps, comm);
            app = app2;
        }
        pr->app = g_strdup(app);
        pr->prev_ticks = ticks;
        g_hash_table_insert(procs, GINT_TO_POINTER(pid), pr);
        g_list_store_append(proc_store, pr);
    }
    g_free(comm);
    /* Share of the whole machine: the process's jiffies over all CPUs'. */
    pr->cpu = total_delta ? 100.0 * (double)(ticks - pr->prev_ticks) / (double)total_delta : 0;
    if (pr->cpu < 0)
        pr->cpu = 0;
    pr->prev_ticks = ticks;
    pr->threads = threads;
    g_snprintf(path, sizeof path, "/proc/%d/statm", pid);
    if (read_small(path, buf, sizeof buf) > 0) {
        char *q = buf;
        g_ascii_strtoull(q, &q, 10);
        pr->rss = g_ascii_strtoull(q, NULL, 10) * (guint64)page_size;
    }
    /* /proc/<pid>/io is readable for our own processes only. */
    if (pr->uid == getuid()) {
        g_snprintf(path, sizeof path, "/proc/%d/io", pid);
        if (read_small(path, buf, sizeof buf) > 0) {
            guint64 rb = 0, wb = 0;
            char *r = strstr(buf, "read_bytes:"), *w = strstr(buf, "\nwrite_bytes:");
            if (r)
                rb = g_ascii_strtoull(r + 11, NULL, 10);
            if (w)
                wb = g_ascii_strtoull(w + 13, NULL, 10);
            pr->io = pr->io_ok ? (double)(rb + wb - pr->prev_io) : 0;
            pr->prev_io = rb + wb;
            pr->io_ok = TRUE;
        }
    }
    pr->gen = scan_gen;
    if (!fresh)
        g_signal_emit(pr, proc_changed, 0);
    return TRUE;
}

static void scan_procs(void)
{
    scan_gen++;
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d)
        return;
    const char *e;
    int count = 0, threads = 0;
    while ((e = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(e[0]))
            continue;
        int pid = atoi(e);
        if (scan_one(pid, S.delta_total)) {
            count++;
            LptProc *pr = g_hash_table_lookup(procs, GINT_TO_POINTER(pid));
            threads += pr ? pr->threads : 1;
        }
    }
    g_dir_close(d);
    S.nprocs = count;
    S.nthreads = threads;
    /* The dead: out of the store and the table. */
    for (guint i = g_list_model_get_n_items(G_LIST_MODEL(proc_store)); i-- > 0;) {
        LptProc *p = g_list_model_get_item(G_LIST_MODEL(proc_store), i);
        if (p->gen != scan_gen) {
            g_list_store_remove(proc_store, i);
            g_hash_table_remove(procs, GINT_TO_POINTER(p->pid));
        }
        g_object_unref(p);
    }
}

/* Counting processes and threads without the full walk, for the CPU page
 * when the process pages are not showing. */
static void count_procs(void)
{
    char buf[4096];
    if (read_small("/proc/loadavg", buf, sizeof buf) > 0) {
        char *slash = strchr(buf, '/');
        if (slash)
            S.nthreads = atoi(slash + 1);
    }
    GDir *d = g_dir_open("/proc", 0, NULL);
    int n = 0;
    const char *e;
    while (d && (e = g_dir_read_name(d)))
        if (g_ascii_isdigit(e[0]))
            n++;
    if (d)
        g_dir_close(d);
    S.nprocs = n;
}

/* ═══════════════════════════════════════════════════════════════════
 * Formatting
 * ═══════════════════════════════════════════════════════════════════ */

static char *fmt_bytes(double b)
{
    if (b < 0)
        return g_strdup("—");
    return g_format_size((guint64)b);
}

static char *fmt_rate(double b)
{
    if (b < 1)
        return g_strdup(T("0 B/s", "0 B/s"));
    char *s = g_format_size((guint64)b);
    char *r = g_strdup_printf("%s/s", s);
    g_free(s);
    return r;
}

static char *fmt_duration(double secs)
{
    long m = (long)(secs / 60);
    long h = m / 60, d = h / 24;
    if (d > 0)
        return g_strdup_printf(T("%ld d %ld h", "%ld일 %ld시간"), d, h % 24);
    if (h > 0)
        return g_strdup_printf(T("%ld h %ld min", "%ld시간 %ld분"), h, m % 60);
    return g_strdup_printf(T("%ld min", "%ld분"), m);
}

/* ═══════════════════════════════════════════════════════════════════
 * The graph (see the file comment: a cached node, translated per frame)
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { FMT_PERCENT, FMT_RATE } GraphFmt;

typedef struct {
    GtkWidget      parent;
    const Series  *a, *b;
    GraphFmt       fmt;
    double         fixed_max;       /* > 0: a fixed scale (percent) */
    LpSpring       value;           /* the headline number, eased */
    LpSpring       scale;           /* auto scale, eased */
    guint          tick;
    gint64         last_frame;
    GskRenderNode *line;
    guint          line_serial;
    int            line_w, line_h;
    double         line_max;
    gint64         line_t;          /* newest sample time in the node */
    double         drawn_off;
    char          *detail;
    char          *caption;
    char          *legend_a, *legend_b;
    PangoLayout   *big, *small, *foot;
    gulong         state_handler;
    GdkSurface    *surface;
} LptGraph;
typedef struct { GtkWidgetClass parent_class; } LptGraphClass;
G_DEFINE_TYPE(LptGraph, lpt_graph, GTK_TYPE_WIDGET)

#define G_PAD    16.0
#define G_HEAD   52.0
#define G_FOOT   26.0

static const GdkRGBA C_LINE_A = { 0.91f, 0.91f, 0.91f, 1.0f };
static const GdkRGBA C_LINE_B = { 0.50f, 0.72f, 0.63f, 1.0f };   /* teal */
static const GdkRGBA C_GRID   = { 1.0f, 1.0f, 1.0f, 0.06f };
static const GdkRGBA C_T1     = { 0.91f, 0.91f, 0.91f, 1.0f };
static const GdkRGBA C_T3     = { 0.42f, 0.42f, 0.42f, 1.0f };
static const GdkRGBA C_AMBER  = { 0.94f, 0.70f, 0.31f, 1.0f };

static double graph_top(LptGraph *g)
{
    if (g->fixed_max > 0)
        return g->fixed_max;
    return MAX(g->scale.x, 1.0);
}

/* A scale that grows to fit and shrinks back slowly: the next power of
 * two (in KiB/s steps) above the largest value on screen. */
static double nice_max(double v)
{
    double m = 64 * 1024;
    while (m < v * 1.15)
        m *= 2;
    return m;
}

static void build_line(LptGraph *g, float pw, float ph)
{
    g_clear_pointer(&g->line, gsk_render_node_unref);
    const Series *ss[2] = { g->a, g->b };
    if (!g->a || g->a->n < 1)
        return;
    double dx = pw / WINDOW_S;
    double top = graph_top(g);
    gint64 tn = g->a->t[g->a->n - 1];
    graphene_rect_t r = GRAPHENE_RECT_INIT(-dx * 2, -2, pw + dx * 4, ph + 4);
    GskRenderNode *node = gsk_cairo_node_new(&r);
    cairo_t *cr = gsk_cairo_node_get_draw_context(node);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int s = 1; s >= 0; s--) {
        const Series *se = ss[s];
        if (!se || se->n < 1)
            continue;
        /* x of a sample: its age before the newest, one dx per second,
         * newest at the right edge (pw). */
        cairo_new_path(cr);
        int first = 1;
        double x0 = 0;
        for (int i = 0; i < se->n; i++) {
            double age = (tn - se->t[i]) / 1e6;
            double x = pw - age * dx;
            if (x < -dx * 2)
                continue;
            double y = ph - CLAMP(se->v[i] / top, 0.0, 1.0) * (ph - 2) - 1;
            if (first) {
                cairo_move_to(cr, x, y);
                x0 = x;
                first = 0;
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        const GdkRGBA *c = s == 0 ? &C_LINE_A : &C_LINE_B;
        if (s == 0 && !first) {
            /* A faint fill under the main line, the mockup's weight. */
            cairo_path_t *path = cairo_copy_path(cr);
            cairo_line_to(cr, pw, ph);
            cairo_line_to(cr, x0, ph);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, c->red, c->green, c->blue, 0.07);
            cairo_fill(cr);
            cairo_append_path(cr, path);
            cairo_path_destroy(path);
        }
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    g->line = node;
    g->line_serial = g->a->serial + (g->b ? g->b->serial * 7919u : 0);
    g->line_w = (int)pw;
    g->line_h = (int)ph;
    g->line_max = top;
    g->line_t = tn;
}

static char *graph_value_text(LptGraph *g, double v)
{
    if (g->fmt == FMT_PERCENT)
        return g_strdup_printf("%.0f%%", CLAMP(v, 0, 100));
    return fmt_rate(v);
}

static void layout_font(PangoLayout *l, int px, int weight)
{
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    /* Tabular digits: a number that changes must not make its
     * neighbours move. */
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_font_features_new("tnum 1"));
    pango_layout_set_attributes(l, al);
    pango_attr_list_unref(al);
}

static void graph_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LptGraph *g = (LptGraph *)w;
    float W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    float px = G_PAD, py = G_HEAD, pw = W - 2 * G_PAD, ph = H - G_HEAD - G_FOOT;
    if (pw < 10 || ph < 10)
        return;
    if (!g->big) {
        g->big = gtk_widget_create_pango_layout(w, "");
        layout_font(g->big, 26, 650);
        g->small = gtk_widget_create_pango_layout(w, "");
        layout_font(g->small, 13, 400);
        g->foot = gtk_widget_create_pango_layout(w, "");
        layout_font(g->foot, 12, 400);
    }

    /* Header: the number, large, and what goes with it. */
    char *vt = graph_value_text(g, g->value.x);
    pango_layout_set_text(g->big, vt, -1);
    g_free(vt);
    int bw, bh;
    pango_layout_get_pixel_size(g->big, &bw, &bh);
    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD, 12));
    gtk_snapshot_append_layout(snap, g->big, &C_T1);
    gtk_snapshot_restore(snap);
    if (g->detail) {
        pango_layout_set_text(g->small, g->detail, -1);
        int sw, sh;
        pango_layout_get_pixel_size(g->small, &sw, &sh);
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD + bw + 10, 12 + bh - sh - 4));
        gtk_snapshot_append_layout(snap, g->small, &C_T3);
        gtk_snapshot_restore(snap);
    }

    /* Three faint lines at quarters. */
    for (int i = 1; i <= 3; i++)
        gtk_snapshot_append_color(snap, &C_GRID,
            &GRAPHENE_RECT_INIT(px, py + ph * i / 4.0f, pw, 1));

    /* The line: rebuilt when a sample arrived, the size or the scale
     * changed; otherwise the same node, moved. */
    guint serial = g->a ? g->a->serial + (g->b ? g->b->serial * 7919u : 0) : 0;
    double top = graph_top(g);
    if (!g->line || g->line_serial != serial || g->line_w != (int)pw ||
        g->line_h != (int)ph || fabs(g->line_max - top) > top * 0.002)
        build_line(g, pw, ph);
    if (g->line) {
        double dx = pw / WINDOW_S;
        /* One sample late: the newest point is at the right edge one
         * second after it was taken, and slides in from beyond it. */
        gint64 now = g->last_frame ? g->last_frame : g_get_monotonic_time();
        double age = (now - g->line_t) / 1e6 - 1.0;
        double off = -CLAMP(age, -1.0, 3.0) * dx;
        g->drawn_off = off;
        gtk_snapshot_push_clip(snap, &GRAPHENE_RECT_INIT(px, py - 2, pw, ph + 4));
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(px + (float)off, py));
        gtk_snapshot_append_node(snap, g->line);
        gtk_snapshot_restore(snap);
        gtk_snapshot_pop(snap);
    }

    /* Footer: what the graph spans, and the legend. */
    GString *ft = g_string_new(g->caption ? g->caption : T("Last 60 seconds", "최근 60초"));
    pango_layout_set_text(g->foot, ft->str, -1);
    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD, H - G_FOOT + 4));
    gtk_snapshot_append_layout(snap, g->foot, &C_T3);
    gtk_snapshot_restore(snap);
    float lx = W - G_PAD;
    const char *leg[2] = { g->legend_b, g->legend_a };
    const GdkRGBA *lc[2] = { &C_LINE_B, &C_LINE_A };
    for (int i = 0; i < 2; i++) {
        if (!leg[i])
            continue;
        pango_layout_set_text(g->foot, leg[i], -1);
        int fw, fh;
        pango_layout_get_pixel_size(g->foot, &fw, &fh);
        lx -= fw;
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(lx, H - G_FOOT + 4));
        gtk_snapshot_append_layout(snap, g->foot, &C_T3);
        gtk_snapshot_restore(snap);
        lx -= 16;
        gtk_snapshot_append_color(snap, lc[i],
            &GRAPHENE_RECT_INIT(lx, H - G_FOOT + 4 + fh / 2.0f - 1, 10, 2));
        lx -= 14;
    }
    g_string_free(ft, TRUE);
}

static gboolean graph_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
    (void)d;
    LptGraph *g = (LptGraph *)w;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = g->last_frame ? (now - g->last_frame) / 1e6 : 0.016;
    g->last_frame = now;
    gboolean moving = lp_spring_step(&g->value, dt);
    moving |= lp_spring_step(&g->scale, dt);
    /* Draw only when the line would move by at least half a pixel on a
     * scale-2 screen, or a number is still easing. */
    float pw = gtk_widget_get_width(w) - 2 * G_PAD;
    double dx = pw / WINDOW_S;
    double off = g->line ? -CLAMP((now - g->line_t) / 1e6 - 1.0, -1.0, 3.0) * dx : 0;
    if (moving || fabs(off - g->drawn_off) >= 0.25)
        gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static gboolean graph_should_tick(LptGraph *g)
{
    if (!gtk_widget_get_mapped(GTK_WIDGET(g)))
        return FALSE;
    GtkNative *nat = gtk_widget_get_native(GTK_WIDGET(g));
    GdkSurface *s = nat ? gtk_native_get_surface(nat) : NULL;
    if (s && GDK_IS_TOPLEVEL(s) &&
        (gdk_toplevel_get_state(GDK_TOPLEVEL(s)) & GDK_TOPLEVEL_STATE_MINIMIZED))
        return FALSE;
    return TRUE;
}

static void graph_update_tick(LptGraph *g)
{
    gboolean want = graph_should_tick(g);
    if (want && !g->tick) {
        g->last_frame = 0;
        g->tick = gtk_widget_add_tick_callback(GTK_WIDGET(g), graph_tick, NULL, NULL);
    } else if (!want && g->tick) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(g), g->tick);
        g->tick = 0;
    }
}

static void on_surface_state(GObject *s, GParamSpec *p, gpointer d)
{
    (void)s; (void)p;
    graph_update_tick(d);
}

static void graph_map(GtkWidget *w)
{
    GTK_WIDGET_CLASS(lpt_graph_parent_class)->map(w);
    LptGraph *g = (LptGraph *)w;
    GtkNative *nat = gtk_widget_get_native(w);
    GdkSurface *s = nat ? gtk_native_get_surface(nat) : NULL;
    if (s && !g->state_handler) {
        g->surface = s;
        g->state_handler = g_signal_connect(s, "notify::state",
                                            G_CALLBACK(on_surface_state), g);
    }
    /* Arriving on a page: the number is already right, not easing up
     * from zero - graphs do not "animate in" (COMMON.md). */
    lp_spring_jump(&g->value, g->a ? series_last(g->a) : 0);
    graph_update_tick(g);
}

static void graph_unmap(GtkWidget *w)
{
    LptGraph *g = (LptGraph *)w;
    if (g->tick) {
        gtk_widget_remove_tick_callback(w, g->tick);
        g->tick = 0;
    }
    if (g->surface && g->state_handler) {
        g_signal_handler_disconnect(g->surface, g->state_handler);
        g->state_handler = 0;
        g->surface = NULL;
    }
    GTK_WIDGET_CLASS(lpt_graph_parent_class)->unmap(w);
}

static void graph_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *mb, int *nb)
{
    (void)w; (void)for_size;
    *min = o == GTK_ORIENTATION_HORIZONTAL ? 200 : 200;
    *nat = o == GTK_ORIENTATION_HORIZONTAL ? 600 : 230;
    *mb = *nb = -1;
}

static void graph_dispose(GObject *o)
{
    LptGraph *g = (LptGraph *)o;
    g_clear_pointer(&g->line, gsk_render_node_unref);
    g_clear_object(&g->big);
    g_clear_object(&g->small);
    g_clear_object(&g->foot);
    g_clear_pointer(&g->detail, g_free);
    g_clear_pointer(&g->caption, g_free);
    g_clear_pointer(&g->legend_a, g_free);
    g_clear_pointer(&g->legend_b, g_free);
    G_OBJECT_CLASS(lpt_graph_parent_class)->dispose(o);
}

static void lpt_graph_class_init(LptGraphClass *k)
{
    G_OBJECT_CLASS(k)->dispose = graph_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = graph_snapshot;
    GTK_WIDGET_CLASS(k)->measure = graph_measure;
    GTK_WIDGET_CLASS(k)->map = graph_map;
    GTK_WIDGET_CLASS(k)->unmap = graph_unmap;
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lptgraph");
}

static void lpt_graph_init(LptGraph *g)
{
    lp_spring_init(&g->value, LP_SPRING_EXPAND, 0);
    lp_spring_init(&g->scale, LP_SPRING_EXPAND, 64 * 1024);
}

static LptGraph *graph_new(const Series *a, const Series *b, GraphFmt fmt,
                           const char *legend_a, const char *legend_b)
{
    LptGraph *g = g_object_new(lpt_graph_get_type(), NULL);
    g->a = a;
    g->b = b;
    g->fmt = fmt;
    g->fixed_max = fmt == FMT_PERCENT ? 100.0 : 0.0;
    g->legend_a = g_strdup(legend_a);
    g->legend_b = g_strdup(legend_b);
    gtk_widget_set_hexpand(GTK_WIDGET(g), TRUE);
    return g;
}

static void graph_sampled(LptGraph *g, const char *detail)
{
    g_free(g->detail);
    g->detail = g_strdup(detail);
    if (!gtk_widget_get_mapped(GTK_WIDGET(g)))
        return;
    lp_spring_set_target(&g->value, g->a ? series_last(g->a) : 0);
    if (g->fixed_max <= 0) {
        double m = 0;
        for (int i = 0; g->a && i < g->a->n; i++)
            m = MAX(m, g->a->v[i]);
        for (int i = 0; g->b && i < g->b->n; i++)
            m = MAX(m, g->b->v[i]);
        lp_spring_set_target(&g->scale, nice_max(m));
    }
    gtk_widget_queue_draw(GTK_WIDGET(g));
}

/* ═══════════════════════════════════════════════════════════════════
 * Bars: per-core usage, and segmented meters
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget  parent;
    int        n;
    LpSpring  *s;
    LpMotion  *m;
    gboolean   meter;        /* one segmented bar instead of n bars */
    GdkRGBA    colors[4];
    PangoLayout *lab;
} LptBars;
typedef struct { GtkWidgetClass parent_class; } LptBarsClass;
G_DEFINE_TYPE(LptBars, lpt_bars, GTK_TYPE_WIDGET)

#define BAR_COLS 8

static void bars_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LptBars *b = (LptBars *)w;
    float W = gtk_widget_get_width(w);
    static const GdkRGBA track = { 1, 1, 1, 0.08f };
    if (b->meter) {
        float H = 10;
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &GRAPHENE_RECT_INIT(0, 0, W, H), 5);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_append_color(snap, &track, &GRAPHENE_RECT_INIT(0, 0, W, H));
        float x = 0;
        for (int i = 0; i < b->n; i++) {
            float sw = (float)CLAMP(b->s[i].x, 0, 1) * W;
            gtk_snapshot_append_color(snap, &b->colors[i], &GRAPHENE_RECT_INIT(x, 0, sw, H));
            x += sw;
        }
        gtk_snapshot_pop(snap);
        return;
    }
    if (!b->lab) {
        b->lab = gtk_widget_create_pango_layout(w, "");
        layout_font(b->lab, 12, 400);
    }
    int cols = MIN(b->n, BAR_COLS);
    float gap = 14, cw = (W - gap * (cols - 1)) / cols;
    for (int i = 0; i < b->n; i++) {
        int r = i / cols, c = i % cols;
        float x = c * (cw + gap), y = r * 40.0f;
        double v = CLAMP(b->s[i].x, 0, 100) / 100.0;
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &GRAPHENE_RECT_INIT(x, y, cw, 6), 3);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_append_color(snap, &track, &GRAPHENE_RECT_INIT(x, y, cw, 6));
        gtk_snapshot_append_color(snap, v > 0.85 ? &C_AMBER : &C_T1,
                                  &GRAPHENE_RECT_INIT(x, y, (float)(cw * v), 6));
        gtk_snapshot_pop(snap);
        char t[16];
        g_snprintf(t, sizeof t, "%d", i + 1);
        pango_layout_set_text(b->lab, t, -1);
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(x, y + 12));
        gtk_snapshot_append_layout(snap, b->lab, &C_T3);
        gtk_snapshot_restore(snap);
    }
}

static void bars_measure(GtkWidget *w, GtkOrientation o, int for_size,
                         int *min, int *nat, int *mb, int *nb)
{
    (void)for_size;
    LptBars *b = (LptBars *)w;
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        *min = 100;
        *nat = 400;
    } else if (b->meter) {
        *min = *nat = 10;
    } else {
        int rows = (b->n + BAR_COLS - 1) / BAR_COLS;
        *min = *nat = rows * 40;
    }
    *mb = *nb = -1;
}

static void bars_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
}

static void bars_dispose(GObject *o)
{
    LptBars *b = (LptBars *)o;
    g_clear_pointer(&b->m, lp_motion_free);
    g_clear_pointer(&b->s, g_free);
    g_clear_object(&b->lab);
    G_OBJECT_CLASS(lpt_bars_parent_class)->dispose(o);
}

static void lpt_bars_class_init(LptBarsClass *k)
{
    G_OBJECT_CLASS(k)->dispose = bars_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = bars_snapshot;
    GTK_WIDGET_CLASS(k)->measure = bars_measure;
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lptbars");
}

static void lpt_bars_init(LptBars *b) { (void)b; }

static LptBars *bars_new(int n, gboolean meter)
{
    LptBars *b = g_object_new(lpt_bars_get_type(), NULL);
    b->n = n;
    b->meter = meter;
    b->s = g_new0(LpSpring, n);
    b->m = lp_motion_new(GTK_WIDGET(b), bars_frame, NULL);
    for (int i = 0; i < n; i++) {
        lp_spring_init(&b->s[i], LP_SPRING_EXPAND, 0);
        lp_motion_add(b->m, &b->s[i]);
    }
    gtk_widget_set_hexpand(GTK_WIDGET(b), TRUE);
    return b;
}

/* Values ease while the bars are on screen and are simply set while they
 * are not - nothing animates for nobody. */
static void bars_set(LptBars *b, int i, double v)
{
    if (i >= b->n)
        return;
    if (!gtk_widget_get_mapped(GTK_WIDGET(b)))
        lp_spring_jump(&b->s[i], v);
    else
        lp_spring_set_target(&b->s[i], v);
}

static void bars_kick(LptBars *b)
{
    if (gtk_widget_get_mapped(GTK_WIDGET(b)))
        lp_motion_kick(b->m);
}

/* ═══════════════════════════════════════════════════════════════════
 * The window
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget *box, *value, *unit, *key;
} Stat;

typedef struct {
    GtkApplication *gapp;
    GtkWidget *win, *sidebar, *stack;
    char      *page;
    GKeyFile  *state;
    gint64     last_sample;

    /* Apps */
    GtkWidget *apps_list;
    GHashTable *app_rows;        /* desktop id -> row widgets (AppRow*) */
    GtkWidget *apps_hint;
    GtkWidget *apps_hint_label;

    /* Processes */
    GtkWidget *proc_view;
    GtkWidget *proc_search;
    GtkCustomFilter *proc_filter;
    GtkSorter *proc_sorter;
    GtkSingleSelection *proc_sel;
    GtkWidget *proc_end, *proc_kill;
    GtkWidget *proc_mine;
    GtkWidget *proc_count;

    /* Resources */
    LptGraph  *g_cpu, *g_mem, *g_gpu, *g_disk, *g_net, *g_bat;
    LptBars   *cores, *mem_bar;
    Stat       st_procs, st_threads, st_uptime, st_temp;
    Stat       st_mem_used, st_mem_avail, st_mem_cache, st_swap;
    Stat       st_gpu_freq, st_gpu_max, st_gpu_drv;
    Stat       st_rd, st_wr;
    Stat       st_rx, st_tx, st_rx_tot, st_tx_tot;
    Stat       st_bat_pct, st_bat_time, st_bat_w, st_bat_health;
    GtkWidget *cpu_sub, *cpu_kv;
    GtkWidget *mem_top;
    GtkWidget *gpu_note;
    GtkWidget *fs_box;
    GtkWidget *net_ifaces;
    GtkWidget *net_wifi;
    GtkWidget *bat_note, *bat_state;
    Series     bat_series;
    guint      slow_count;
} App;

static App *A;

static GtkWindow *win(void) { return GTK_WINDOW(A->win); }

static gboolean page_is(const char *id)
{
    const char *v = gtk_stack_get_visible_child_name(GTK_STACK(A->stack));
    return v && !strcmp(v, id) && gtk_widget_get_mapped(A->stack);
}

static gboolean window_minimised(void)
{
    GtkNative *nat = GTK_NATIVE(A->win);
    GdkSurface *s = gtk_native_get_surface(nat);
    return !gtk_widget_get_mapped(A->win) ||
           (s && GDK_IS_TOPLEVEL(s) &&
            (gdk_toplevel_get_state(GDK_TOPLEVEL(s)) & GDK_TOPLEVEL_STATE_MINIMIZED));
}

/* ── small builders ───────────────────────────────────────────────── */

static GtkWidget *label(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    if (css)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static GtkWidget *page_head(const char *title, const char *sub, GtkWidget **sub_out)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_append(GTK_BOX(b), label(title, "lpt-h1", 0));
    GtkWidget *s = label(sub ? sub : "", "lpt-sub", 0);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    gtk_box_append(GTK_BOX(b), s);
    if (sub_out)
        *sub_out = s;
    gtk_widget_set_margin_bottom(b, 12);
    return b;
}

static GtkWidget *section(const char *text)
{
    GtkWidget *l = label(text, "lpt-sec", 0);
    gtk_widget_set_margin_top(l, 18);
    gtk_widget_set_margin_bottom(l, 6);
    return l;
}

static GtkWidget *stat_new(Stat *st, const char *key)
{
    st->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(st->box, "lpt-stat");
    gtk_widget_set_hexpand(st->box, TRUE);
    st->key = label(key, "lpt-stat-k", 0);
    gtk_box_append(GTK_BOX(st->box), st->key);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    st->value = label("—", "lpt-stat-v", 0);
    st->unit = label("", "lpt-stat-u", 0);
    gtk_widget_set_valign(st->unit, GTK_ALIGN_BASELINE);
    gtk_widget_set_valign(st->value, GTK_ALIGN_BASELINE);
    gtk_box_append(GTK_BOX(row), st->value);
    gtk_box_append(GTK_BOX(row), st->unit);
    gtk_box_append(GTK_BOX(st->box), row);
    return st->box;
}

static void stat_set(Stat *st, const char *value, const char *unit, gboolean warn)
{
    gtk_label_set_text(GTK_LABEL(st->value), value);
    gtk_label_set_text(GTK_LABEL(st->unit), unit ? unit : "");
    if (warn)
        gtk_widget_add_css_class(st->box, "warn");
    else
        gtk_widget_remove_css_class(st->box, "warn");
}

/* "9.8 GB" -> value "9.8", unit "GB" */
static void stat_bytes(Stat *st, double b, gboolean rate)
{
    char *s = rate ? fmt_rate(b) : fmt_bytes(b);
    char *sp = strchr(s, ' ');
    if (sp) {
        *sp = '\0';
        stat_set(st, s, sp + 1, FALSE);
    } else {
        stat_set(st, s, "", FALSE);
    }
    g_free(s);
}

static GtkWidget *stats_row(GtkWidget *a, GtkWidget *b, GtkWidget *c, GtkWidget *d)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_widget_set_margin_top(g, 12);
    GtkWidget *w[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++)
        if (w[i])
            gtk_grid_attach(GTK_GRID(g), w[i], i, 0, 1, 1);
    return g;
}

static GtkWidget *kv_row(GtkWidget *box, const char *k, const char *v)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(r, "lpt-kv");
    GtkWidget *kl = label(k, "lpt-kv-k", 0);
    gtk_widget_set_hexpand(kl, TRUE);
    GtkWidget *vl = label(v, "lpt-kv-v", 1);
    gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
    gtk_box_append(GTK_BOX(r), kl);
    gtk_box_append(GTK_BOX(r), vl);
    gtk_box_append(GTK_BOX(box), r);
    return vl;
}

static GtkWidget *page_box(void)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(b, "lpt-page");
    return b;
}

/* ── Apps ─────────────────────────────────────────────────────────── */

typedef struct {
    char      *id;
    GtkWidget *rev, *row, *icon, *name, *count, *cpu, *mem;
    double     cpu_v;
    guint64    mem_v;
    int        n;
    gboolean   leaving;
    GPtrArray *pids;
} AppRow;

static void approw_free(gpointer d)
{
    AppRow *r = d;
    g_free(r->id);
    g_ptr_array_unref(r->pids);
    g_free(r);
}

typedef struct { int *pids; int n; int sig; char *name; } KillReq;

static void do_kill(KillReq *k)
{
    int failed = 0, last_err = 0;
    for (int i = 0; i < k->n; i++)
        if (kill(k->pids[i], k->sig) != 0) {
            failed++;
            last_err = errno;
        }
    if (failed) {
        char *msg = g_strdup_printf(
            last_err == EPERM
            ? T("%s belongs to another account. Only its owner or an "
                "administrator in a terminal (sudo kill) can end it.",
                "%s 은(는) 다른 계정의 것입니다. 그 계정이나 관리자가 터미널에서 "
                "(sudo kill) 끝낼 수 있습니다.")
            : T("%s could not be ended: it may have ended already.",
                "%s 을(를) 끝내지 못했습니다. 이미 끝났을 수 있습니다."),
            k->name);
        LpKitDialog *d = lp_kit_dialog_new(win(), T("Not ended", "끝내지 못함"), msg);
        lp_kit_dialog_button(d, T("OK", "확인"), LP_KIT_OK, "suggested-action");
        lp_kit_dialog_present(d);
        g_free(msg);
    }
}

static void on_kill_confirmed(gboolean ok, gpointer data)
{
    KillReq *k = data;
    if (ok)
        do_kill(k);
    g_free(k->pids);
    g_free(k->name);
    g_free(k);
}

static void ask_kill(const char *name, int *pids, int n, int sig)
{
    KillReq *k = g_new0(KillReq, 1);
    k->pids = g_memdup2(pids, sizeof(int) * (gsize)n);
    k->n = n;
    k->sig = sig;
    k->name = g_strdup(name);
    char *t = g_strdup_printf(sig == SIGKILL ? T("Kill %s?", "%s 을(를) 강제로 끝낼까요?")
                                             : T("End %s?", "%s 을(를) 끝낼까요?"), name);
    const char *body = sig == SIGKILL
        ? T("It stops at once, without a chance to save. Anything not saved is lost.",
            "저장할 기회 없이 즉시 멈춥니다. 저장하지 않은 것은 사라집니다.")
        : T("It is asked to close. Most programs save and quit; unsaved work "
            "may still be lost.",
            "닫으라고 요청합니다. 대부분 저장하고 끝나지만 저장하지 않은 작업은 "
            "사라질 수 있습니다.");
    lp_kit_confirm(win(), t, body, sig == SIGKILL ? T("Kill", "강제 종료") : T("End", "끝내기"),
                   TRUE, on_kill_confirmed, k);
    g_free(t);
}

static void on_app_end(GtkButton *b, gpointer d)
{
    (void)b;
    AppRow *r = d;
    if (!r->pids->len)
        return;
    int *p = g_new(int, r->pids->len);
    for (guint i = 0; i < r->pids->len; i++)
        p[i] = GPOINTER_TO_INT(g_ptr_array_index(r->pids, i));
    ask_kill(gtk_label_get_text(GTK_LABEL(r->name)), p, (int)r->pids->len, SIGTERM);
    g_free(p);
}

static AppRow *approw_new(const char *id)
{
    AppRow *r = g_new0(AppRow, 1);
    r->id = g_strdup(id);
    r->pids = g_ptr_array_new();
    GDesktopAppInfo *ai = g_desktop_app_info_new(id);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(box, "lpt-app-row");
    r->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(r->icon), 32);
    GIcon *ic = ai ? g_app_info_get_icon(G_APP_INFO(ai)) : NULL;
    if (ic)
        gtk_image_set_from_gicon(GTK_IMAGE(r->icon), ic);
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(r->icon), "application-x-executable");
    gtk_box_append(GTK_BOX(box), r->icon);
    GtkWidget *nb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(nb, TRUE);
    r->name = label(ai ? g_app_info_get_display_name(G_APP_INFO(ai)) : id, "lpt-app-name", 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
    r->count = label("", "lpt-dim", 0);
    gtk_box_append(GTK_BOX(nb), r->name);
    gtk_box_append(GTK_BOX(nb), r->count);
    gtk_box_append(GTK_BOX(box), nb);
    r->cpu = label("", "lpt-num", 1);
    gtk_widget_set_size_request(r->cpu, 80, -1);
    r->mem = label("", "lpt-num", 1);
    gtk_widget_set_size_request(r->mem, 100, -1);
    gtk_box_append(GTK_BOX(box), r->cpu);
    gtk_box_append(GTK_BOX(box), r->mem);
    GtkWidget *end = gtk_button_new_with_label(T("End", "끝내기"));
    gtk_widget_add_css_class(end, "lpt-action");
    char *nm = g_strdup_printf("end-%s", id);
    gtk_widget_set_name(end, nm);
    g_free(nm);
    g_signal_connect(end, "clicked", G_CALLBACK(on_app_end), r);
    gtk_box_append(GTK_BOX(box), end);
    r->row = box;
    if (ai)
        g_object_unref(ai);
    return r;
}

static int apps_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer d)
{
    (void)d;
    AppRow *x = g_object_get_data(G_OBJECT(a), "approw");
    AppRow *y = g_object_get_data(G_OBJECT(b), "approw");
    if (!x || !y)
        return 0;
    /* Bands of 1%, so two apps near each other do not swap places every
     * second; then by name. */
    int cx = (int)x->cpu_v, cy = (int)y->cpu_v;
    if (cx != cy)
        return cy - cx;
    return g_utf8_collate(gtk_label_get_text(GTK_LABEL(x->name)),
                          gtk_label_get_text(GTK_LABEL(y->name)));
}

static void approw_gone(GtkWidget *rev, gpointer d)
{
    (void)d;
    GtkWidget *row = gtk_widget_get_parent(rev);
    if (row && GTK_IS_LIST_BOX_ROW(row))
        gtk_list_box_remove(GTK_LIST_BOX(A->apps_list), row);
}

static void update_apps(void)
{
    GHashTable *sum = g_hash_table_new(g_str_hash, g_str_equal);
    guint me = getuid();
    GHashTableIter it;
    gpointer k, v;
    for (GHashTableIter *p = (g_hash_table_iter_init(&it, A->app_rows), &it);
         g_hash_table_iter_next(p, &k, &v);) {
        AppRow *r = v;
        r->cpu_v = 0;
        r->mem_v = 0;
        r->n = 0;
        g_ptr_array_set_size(r->pids, 0);
    }
    gboolean first = g_hash_table_size(A->app_rows) == 0;
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        LptProc *pr = v;
        if (!pr->app || pr->uid != me)
            continue;
        AppRow *r = g_hash_table_lookup(A->app_rows, pr->app);
        if (!r) {
            r = approw_new(pr->app);
            g_hash_table_insert(A->app_rows, r->id, r);
            r->rev = lp_kit_reveal_in(r->row, first);
            gtk_list_box_append(GTK_LIST_BOX(A->apps_list), r->rev);
            GtkWidget *lbr = gtk_widget_get_parent(r->rev);
            g_object_set_data(G_OBJECT(lbr), "approw", r);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbr), FALSE);
        }
        r->cpu_v += pr->cpu;
        r->mem_v += pr->rss;
        r->n++;
        g_ptr_array_add(r->pids, GINT_TO_POINTER(pr->pid));
        g_hash_table_add(sum, r->id);
    }
    AppRow *top = NULL;
    GPtrArray *gone = g_ptr_array_new();
    g_hash_table_iter_init(&it, A->app_rows);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        AppRow *r = v;
        if (!g_hash_table_contains(sum, r->id)) {
            if (!r->leaving) {
                r->leaving = TRUE;
                g_ptr_array_add(gone, r);
            }
            continue;
        }
        char t[64];
        g_snprintf(t, sizeof t, "%.1f%%", r->cpu_v);
        gtk_label_set_text(GTK_LABEL(r->cpu), t);
        if (r->cpu_v >= 25)
            gtk_widget_add_css_class(r->cpu, "hot");
        else
            gtk_widget_remove_css_class(r->cpu, "hot");
        char *m = fmt_bytes((double)r->mem_v);
        gtk_label_set_text(GTK_LABEL(r->mem), m);
        g_free(m);
        char *c = r->n > 1 ? g_strdup_printf(T("%d processes", "프로세스 %d개"), r->n) : g_strdup("");
        gtk_label_set_text(GTK_LABEL(r->count), c);
        g_free(c);
        if (!top || r->cpu_v > top->cpu_v)
            top = r;
    }
    for (guint i = 0; i < gone->len; i++) {
        AppRow *r = g_ptr_array_index(gone, i);
        g_hash_table_steal(A->app_rows, r->id);
        lp_kit_reveal_out(r->rev, G_CALLBACK(approw_gone), NULL);
        g_object_set_data_full(G_OBJECT(r->rev), "approw-free", r, approw_free);
    }
    g_ptr_array_unref(gone);
    g_hash_table_unref(sum);
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(A->apps_list));
    /* The spec's "a sentence next to the number". */
    if (top && top->cpu_v >= 25) {
        char *h = g_strdup_printf(T("▲ %s is using the most CPU", "▲ %s 이(가) CPU를 가장 많이 쓰고 있습니다"),
                                  gtk_label_get_text(GTK_LABEL(top->name)));
        gtk_label_set_text(GTK_LABEL(A->apps_hint_label), h);
        g_free(h);
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->apps_hint), TRUE);
    } else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->apps_hint), FALSE);
    }
}

static GtkWidget *build_apps(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Apps", "앱"),
        T("Applications you are running", "지금 실행 중인 프로그램입니다"), NULL));
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(hdr, "lpt-colhead");
    GtkWidget *hn = label(T("Name", "이름"), NULL, 0);
    gtk_widget_set_hexpand(hn, TRUE);
    gtk_widget_set_margin_start(hn, 46);
    gtk_box_append(GTK_BOX(hdr), hn);
    GtkWidget *hc = label("CPU", NULL, 1);
    gtk_widget_set_size_request(hc, 80, -1);
    GtkWidget *hm = label(T("Memory", "메모리"), NULL, 1);
    gtk_widget_set_size_request(hm, 100, -1);
    GtkWidget *he = label("", NULL, 1);
    gtk_widget_set_size_request(he, 96, -1);
    gtk_box_append(GTK_BOX(hdr), hc);
    gtk_box_append(GTK_BOX(hdr), hm);
    gtk_box_append(GTK_BOX(hdr), he);
    gtk_box_append(GTK_BOX(b), hdr);
    A->apps_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A->apps_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(A->apps_list, "lpt-list");
    gtk_list_box_set_sort_func(GTK_LIST_BOX(A->apps_list), apps_sort, NULL, NULL);
    gtk_box_append(GTK_BOX(b), A->apps_list);
    A->apps_hint_label = label("", "lpt-hint", 0);
    A->apps_hint = gtk_revealer_new();
    gtk_revealer_set_transition_duration(GTK_REVEALER(A->apps_hint),
                                         lp_spring_ms(LP_SPRING_EXPAND, FALSE));
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(hb, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hb, 16);
    gtk_box_append(GTK_BOX(hb), A->apps_hint_label);
    gtk_revealer_set_child(GTK_REVEALER(A->apps_hint), hb);
    gtk_box_append(GTK_BOX(b), A->apps_hint);
    GtkWidget *note = label(T("Only programs with an application entry are listed here. "
                              "Everything else is under Processes.",
                              "앱 탭에는 앱 항목이 있는 프로그램만 보입니다. 나머지는 "
                              "프로세스 탭에 있습니다."), "lpt-note", 0);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_margin_top(note, 14);
    gtk_box_append(GTK_BOX(b), note);
    return lp_kit_scroller(b, FALSE);
}

/* ── Processes ────────────────────────────────────────────────────── */

typedef enum { COL_NAME, COL_PID, COL_USER, COL_CPU, COL_MEM, COL_DISK } Col;

typedef struct {
    GtkWidget *l;
    LptProc   *p;
    gulong     h;
    Col        col;
} Cell;

static void cell_fill(Cell *c)
{
    LptProc *p = c->p;
    char t[64];
    switch (c->col) {
    case COL_NAME: gtk_label_set_text(GTK_LABEL(c->l), p->name); break;
    case COL_PID:  g_snprintf(t, sizeof t, "%d", p->pid); gtk_label_set_text(GTK_LABEL(c->l), t); break;
    case COL_USER: gtk_label_set_text(GTK_LABEL(c->l), p->user); break;
    case COL_CPU:
        g_snprintf(t, sizeof t, "%.1f%%", p->cpu);
        gtk_label_set_text(GTK_LABEL(c->l), t);
        if (p->cpu >= 25) gtk_widget_add_css_class(c->l, "hot");
        else gtk_widget_remove_css_class(c->l, "hot");
        break;
    case COL_MEM: {
        char *s = fmt_bytes((double)p->rss);
        gtk_label_set_text(GTK_LABEL(c->l), s);
        g_free(s);
        break;
    }
    case COL_DISK: {
        char *s = p->io < 0 ? g_strdup("—") : fmt_rate(p->io);
        gtk_label_set_text(GTK_LABEL(c->l), s);
        g_free(s);
        break;
    }
    }
}

static void on_proc_changed(LptProc *p, gpointer d)
{
    (void)p;
    cell_fill(d);
}

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f;
    Cell *c = g_new0(Cell, 1);
    c->col = GPOINTER_TO_INT(col);
    c->l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->l), c->col == COL_NAME || c->col == COL_USER ? 0 : 1);
    gtk_label_set_ellipsize(GTK_LABEL(c->l), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(c->l, c->col == COL_NAME ? "lpt-cell-name" : "lpt-num");
    g_object_set_data_full(G_OBJECT(c->l), "cell", c, g_free);
    gtk_list_item_set_child(li, c->l);
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f; (void)col;
    Cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "cell");
    c->p = gtk_list_item_get_item(li);
    c->h = g_signal_connect(c->p, "changed", G_CALLBACK(on_proc_changed), c);
    cell_fill(c);
}

static void cell_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f; (void)col;
    Cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "cell");
    if (c->p && c->h)
        g_signal_handler_disconnect(c->p, c->h);
    c->p = NULL;
    c->h = 0;
}

static int cmp_name(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return g_ascii_strcasecmp(((LptProc *)a)->name, ((LptProc *)b)->name);
}
static int cmp_pid(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return ((LptProc *)a)->pid - ((LptProc *)b)->pid;
}
static int cmp_user(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return g_strcmp0(((LptProc *)a)->user, ((LptProc *)b)->user);
}
static int cmp_cpu(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    double x = ((LptProc *)a)->cpu, y = ((LptProc *)b)->cpu;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}
static int cmp_mem(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    guint64 x = ((LptProc *)a)->rss, y = ((LptProc *)b)->rss;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}
static int cmp_disk(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    double x = ((LptProc *)a)->io, y = ((LptProc *)b)->io;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}

static gboolean proc_visible(gpointer item, gpointer d)
{
    (void)d;
    LptProc *p = item;
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(A->proc_mine)) && p->uid != getuid())
        return FALSE;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(A->proc_search));
    if (!q || !*q)
        return TRUE;
    char *lq = g_utf8_strdown(q, -1);
    char *ln = g_utf8_strdown(p->cmd ? p->cmd : p->name, -1);
    char pid[16];
    g_snprintf(pid, sizeof pid, "%d", p->pid);
    gboolean hit = strstr(ln, lq) || strstr(p->name, q) || !strcmp(pid, q);
    g_free(lq);
    g_free(ln);
    return hit;
}

static void on_proc_search(GtkEditable *e, gpointer d)
{
    (void)e; (void)d;
    gtk_filter_changed(GTK_FILTER(A->proc_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void on_mine_toggled(GtkCheckButton *b, gpointer d)
{
    (void)d;
    gtk_filter_changed(GTK_FILTER(A->proc_filter), GTK_FILTER_CHANGE_DIFFERENT);
    g_key_file_set_boolean(A->state, "tasks", "only-mine", gtk_check_button_get_active(b));
    lp_kit_state_save(A->state, STATE_NAME);
}

static LptProc *selected_proc(void)
{
    return gtk_single_selection_get_selected_item(A->proc_sel);
}

static void on_proc_selected(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p; (void)d;
    LptProc *pr = selected_proc();
    gtk_widget_set_sensitive(A->proc_end, pr != NULL);
    gtk_widget_set_sensitive(A->proc_kill, pr != NULL);
}

static void on_proc_end(GtkButton *b, gpointer d)
{
    (void)b;
    LptProc *pr = selected_proc();
    if (!pr)
        return;
    int pid = pr->pid;
    char *n = g_strdup_printf("%s (%d)", pr->name, pid);
    ask_kill(n, &pid, 1, GPOINTER_TO_INT(d));
    g_free(n);
}

static void proc_context(GtkWidget *w, double x, double y, gpointer d)
{
    (void)d;
    LptProc *pr = selected_proc();
    if (!pr)
        return;
    GMenu *m = g_menu_new();
    g_menu_append(m, T("End", "끝내기"), "win.proc-end");
    g_menu_append(m, T("Kill", "강제 종료"), "win.proc-kill");
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    gtk_widget_set_parent(pop, w);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(m);
}

static void act_proc(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p;
    on_proc_end(NULL, d);
}

static void on_sort_changed(GtkSorter *s, GtkSorterChange c, gpointer d)
{
    (void)c; (void)d;
    GtkColumnViewColumn *col = gtk_column_view_sorter_get_primary_sort_column(
        GTK_COLUMN_VIEW_SORTER(s));
    if (!col)
        return;
    g_key_file_set_string(A->state, "tasks", "sort",
                          gtk_column_view_column_get_id(col) ?
                          gtk_column_view_column_get_id(col) : "cpu");
    g_key_file_set_boolean(A->state, "tasks", "sort-desc",
        gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(s)) ==
        GTK_SORT_DESCENDING);
    lp_kit_state_save(A->state, STATE_NAME);
}

static GtkWidget *build_processes(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Processes", "프로세스"),
        T("Everything running on this computer", "이 컴퓨터에서 실행 중인 모든 것"), NULL));

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    A->proc_search = gtk_search_entry_new();
    g_object_set(A->proc_search, "placeholder-text",
                 T("Search by name, command or PID", "이름, 명령, PID로 찾기"), NULL);
    gtk_widget_add_css_class(A->proc_search, "lpt-search");
    gtk_widget_set_name(A->proc_search, "proc-search");
    gtk_widget_set_hexpand(A->proc_search, TRUE);
    g_signal_connect(A->proc_search, "search-changed", G_CALLBACK(on_proc_search), NULL);
    gtk_box_append(GTK_BOX(bar), A->proc_search);
    A->proc_mine = gtk_check_button_new_with_label(T("Only mine", "내 것만"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(A->proc_mine),
        g_key_file_get_boolean(A->state, "tasks", "only-mine", NULL));
    g_signal_connect(A->proc_mine, "toggled", G_CALLBACK(on_mine_toggled), NULL);
    gtk_box_append(GTK_BOX(bar), A->proc_mine);
    A->proc_end = lp_kit_button(NULL, T("End", "끝내기"), NULL);
    gtk_widget_set_name(A->proc_end, "proc-end");
    A->proc_kill = lp_kit_button(NULL, T("Kill", "강제 종료"), "destructive-action");
    gtk_widget_set_sensitive(A->proc_end, FALSE);
    gtk_widget_set_sensitive(A->proc_kill, FALSE);
    g_signal_connect(A->proc_end, "clicked", G_CALLBACK(on_proc_end), GINT_TO_POINTER(SIGTERM));
    g_signal_connect(A->proc_kill, "clicked", G_CALLBACK(on_proc_end), GINT_TO_POINTER(SIGKILL));
    gtk_box_append(GTK_BOX(bar), A->proc_end);
    gtk_box_append(GTK_BOX(bar), A->proc_kill);
    gtk_box_append(GTK_BOX(b), bar);

    A->proc_filter = gtk_custom_filter_new(proc_visible, NULL, NULL);
    GtkFilterListModel *fm = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(proc_store)),
                                                       GTK_FILTER(A->proc_filter));
    GtkWidget *cv = gtk_column_view_new(NULL);
    gtk_widget_add_css_class(cv, "lpt-procs");
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(cv), FALSE);
    GtkSortListModel *sm = gtk_sort_list_model_new(G_LIST_MODEL(fm),
        g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv))));
    A->proc_sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv));
    A->proc_sel = gtk_single_selection_new(G_LIST_MODEL(sm));
    gtk_single_selection_set_autoselect(A->proc_sel, FALSE);
    gtk_single_selection_set_can_unselect(A->proc_sel, TRUE);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(cv), GTK_SELECTION_MODEL(A->proc_sel));
    g_signal_connect(A->proc_sel, "notify::selected", G_CALLBACK(on_proc_selected), NULL);

    static const struct { const char *id, *en, *ko; GCompareDataFunc cmp; int w; } COLS[] = {
        { "name", "Name", "이름", cmp_name, 0 },
        { "pid", "PID", "PID", cmp_pid, 90 },
        { "user", "User", "사용자", cmp_user, 110 },
        { "cpu", "CPU", "CPU", cmp_cpu, 90 },
        { "mem", "Memory", "메모리", cmp_mem, 110 },
        { "disk", "Disk", "디스크", cmp_disk, 110 },
    };
    char *want = g_key_file_get_string(A->state, "tasks", "sort", NULL);
    gboolean desc = g_key_file_has_key(A->state, "tasks", "sort-desc", NULL)
        ? g_key_file_get_boolean(A->state, "tasks", "sort-desc", NULL) : TRUE;
    GtkColumnViewColumn *sort_col = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(COLS); i++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(cell_setup), GINT_TO_POINTER(i));
        g_signal_connect(f, "bind", G_CALLBACK(cell_bind), GINT_TO_POINTER(i));
        g_signal_connect(f, "unbind", G_CALLBACK(cell_unbind), GINT_TO_POINTER(i));
        GtkColumnViewColumn *c = gtk_column_view_column_new(T(COLS[i].en, COLS[i].ko), f);
        gtk_column_view_column_set_id(c, COLS[i].id);
        gtk_column_view_column_set_sorter(c,
            GTK_SORTER(gtk_custom_sorter_new(COLS[i].cmp, NULL, NULL)));
        if (COLS[i].w)
            gtk_column_view_column_set_fixed_width(c, COLS[i].w);
        else
            gtk_column_view_column_set_expand(c, TRUE);
        gtk_column_view_column_set_resizable(c, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(cv), c);
        if ((want && !strcmp(want, COLS[i].id)) || (!want && !strcmp(COLS[i].id, "cpu")))
            sort_col = c;
        g_object_unref(c);
    }
    g_free(want);
    if (sort_col)
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(cv), sort_col,
                                       desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    g_signal_connect(A->proc_sorter, "changed", G_CALLBACK(on_sort_changed), NULL);
    lp_kit_context(cv, proc_context, NULL);
    A->proc_view = cv;
    GtkWidget *sc = lp_kit_scroller(cv, FALSE);
    gtk_widget_set_margin_top(sc, 12);
    gtk_box_append(GTK_BOX(b), sc);
    A->proc_count = label("", "lpt-note", 0);
    gtk_widget_set_margin_top(A->proc_count, 8);
    gtk_box_append(GTK_BOX(b), A->proc_count);
    return b;
}

/* ── CPU ──────────────────────────────────────────────────────────── */

static GtkWidget *build_cpu(void)
{
    GtkWidget *b = page_box();
    char *sub = g_strdup_printf(T("%s · %d cores, %d threads", "%s · %d코어 %d스레드"),
                                S.model, S.cores, S.ncpu);
    gtk_box_append(GTK_BOX(b), page_head("CPU", sub, &A->cpu_sub));
    g_free(sub);
    A->g_cpu = graph_new(&S.cpu_h, NULL, FMT_PERCENT, NULL, NULL);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_cpu));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_procs, T("Processes", "프로세스")),
        stat_new(&A->st_threads, T("Threads", "스레드")),
        stat_new(&A->st_uptime, T("Up for", "가동 시간")),
        stat_new(&A->st_temp, T("Temperature", "온도"))));
    gtk_box_append(GTK_BOX(b), section(T("Usage per core", "코어별 사용률")));
    A->cores = bars_new(S.ncpu, FALSE);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->cores));
    A->cpu_kv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(A->cpu_kv, 10);
    char t[64];
    if (S.base_mhz > 0) {
        g_snprintf(t, sizeof t, "%.2f GHz", S.base_mhz / 1000);
        kv_row(A->cpu_kv, T("Base clock", "기본 클럭"), t);
    }
    if (S.max_mhz > 0) {
        g_snprintf(t, sizeof t, "%.2f GHz", S.max_mhz / 1000);
        kv_row(A->cpu_kv, T("Maximum clock", "최대 클럭"), t);
    }
    if (S.l3)
        kv_row(A->cpu_kv, T("L3 cache", "L3 캐시"), S.l3);
    kv_row(A->cpu_kv, T("Virtualisation", "가상화"),
           S.virt ? T("Available", "사용 가능") : T("Not available", "사용 불가"));
    gtk_box_append(GTK_BOX(b), A->cpu_kv);
    return lp_kit_scroller(b, FALSE);
}

/* ── Memory ───────────────────────────────────────────────────────── */

static GtkWidget *legend_dot(const char *text, const char *css)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "lpt-dot");
    gtk_widget_add_css_class(d, css);
    gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), d);
    gtk_box_append(GTK_BOX(b), label(text, "lpt-note", 0));
    return b;
}

static GtkWidget *build_memory(void)
{
    GtkWidget *b = page_box();
    char *tot = fmt_bytes((double)S.mem_total);
    char *sub = g_strdup_printf(T("%s installed", "%s 설치됨"), tot);
    gtk_box_append(GTK_BOX(b), page_head(T("Memory", "메모리"), sub, NULL));
    g_free(sub);
    g_free(tot);
    A->g_mem = graph_new(&S.mem_h, NULL, FMT_PERCENT, NULL, NULL);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_mem));
    gtk_box_append(GTK_BOX(b), section(T("Where it is", "쓰임새")));
    A->mem_bar = bars_new(2, TRUE);
    A->mem_bar->colors[0] = C_LINE_A;
    A->mem_bar->colors[1] = (GdkRGBA){ 0.50f, 0.72f, 0.63f, 0.8f };
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->mem_bar));
    GtkWidget *leg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(leg, 8);
    gtk_box_append(GTK_BOX(leg), legend_dot(T("In use", "사용 중"), "a"));
    gtk_box_append(GTK_BOX(leg), legend_dot(T("Cache (given back when needed)",
                                              "캐시 (필요하면 돌려줌)"), "b"));
    gtk_box_append(GTK_BOX(leg), legend_dot(T("Free", "여유"), "c"));
    gtk_box_append(GTK_BOX(b), leg);
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_mem_used, T("In use", "사용 중")),
        stat_new(&A->st_mem_avail, T("Available", "사용 가능")),
        stat_new(&A->st_mem_cache, T("Cache", "캐시")),
        stat_new(&A->st_swap, T("Swap used", "스왑 사용"))));
    gtk_box_append(GTK_BOX(b), section(T("Using the most", "가장 많이 쓰는 것")));
    A->mem_top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(b), A->mem_top);
    return lp_kit_scroller(b, FALSE);
}

static void update_mem_top(void)
{
    GPtrArray *arr = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(arr, v);
    g_ptr_array_sort_with_data(arr, (GCompareDataFunc)(void *)NULL == NULL ? NULL : NULL, NULL);
    /* Top five by resident memory; a tiny selection, done by hand. */
    LptProc *top[5] = { 0 };
    for (guint i = 0; i < arr->len; i++) {
        LptProc *p = g_ptr_array_index(arr, i);
        for (int j = 0; j < 5; j++)
            if (!top[j] || p->rss > top[j]->rss) {
                memmove(top + j + 1, top + j, sizeof(top[0]) * (size_t)(4 - j));
                top[j] = p;
                break;
            }
    }
    g_ptr_array_unref(arr);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->mem_top)))
        gtk_box_remove(GTK_BOX(A->mem_top), c);
    for (int j = 0; j < 5 && top[j]; j++) {
        char *m = fmt_bytes((double)top[j]->rss);
        kv_row(A->mem_top, top[j]->name, m);
        g_free(m);
    }
}

/* ── GPU ──────────────────────────────────────────────────────────── */

static GtkWidget *build_gpu(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head("GPU",
        S.gpu_driver ? (strcmp(S.gpu_driver, "i915") == 0
                        ? T("Intel graphics (i915) - the one the screen is wired to",
                            "인텔 그래픽 (i915) - 화면이 연결된 GPU")
                        : S.gpu_driver)
                     : T("No graphics driver readings on this computer",
                         "이 컴퓨터에서 그래픽 드라이버 정보를 읽을 수 없습니다"), NULL));
    A->g_gpu = graph_new(&S.gpu_h, NULL, FMT_PERCENT, NULL, NULL);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_gpu));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_gpu_freq, T("Clock now", "현재 클럭")),
        stat_new(&A->st_gpu_max, T("Maximum", "최대 클럭")),
        stat_new(&A->st_gpu_drv, T("Driver", "드라이버")), NULL));
    A->gpu_note = label("", "lpt-note", 0);
    gtk_label_set_wrap(GTK_LABEL(A->gpu_note), TRUE);
    gtk_widget_set_margin_top(A->gpu_note, 14);
    gtk_box_append(GTK_BOX(b), A->gpu_note);
    return lp_kit_scroller(b, FALSE);
}

/* ── Drives ───────────────────────────────────────────────────────── */

static GtkWidget *build_drives(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Drives", "드라이브"),
        T("How fast the disks are being read and written, and how full they are",
          "디스크를 읽고 쓰는 속도와 남은 공간"), NULL));
    A->g_disk = graph_new(&S.rd_h, &S.wr_h, FMT_RATE, T("Read", "읽기"), T("Write", "쓰기"));
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_disk));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_rd, T("Reading", "읽기")),
        stat_new(&A->st_wr, T("Writing", "쓰기")), NULL, NULL));
    gtk_box_append(GTK_BOX(b), section(T("Space", "공간")));
    A->fs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(b), A->fs_box);
    GtkWidget *n = label(T("Drive health (SMART) is in Disks.", "드라이브 상태(SMART)는 디스크 앱에 있습니다."),
                         "lpt-note", 0);
    gtk_widget_set_margin_top(n, 14);
    gtk_box_append(GTK_BOX(b), n);
    return lp_kit_scroller(b, FALSE);
}

static void update_fs(void)
{
    sample_fs();
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->fs_box)))
        gtk_box_remove(GTK_BOX(A->fs_box), c);
    for (guint i = 0; i < S.fs->len; i++) {
        Fs *f = g_ptr_array_index(S.fs, i);
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(card, "lpt-fs");
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *n = label(f->name, "lpt-app-name", 0);
        gtk_box_append(GTK_BOX(top), n);
        gtk_box_append(GTK_BOX(top), label(f->dev, "lpt-dim", 0));
        GtkWidget *sp = label("", NULL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(top), sp);
        char *av = fmt_bytes((double)f->avail), *sz = fmt_bytes((double)f->size);
        char *t = g_strdup_printf(T("%s free of %s", "%s 중 %s 남음"),
                                  lp_korean() ? sz : av, lp_korean() ? av : sz);
        gboolean low = f->size && (double)f->avail / (double)f->size < LOW_FREE;
        GtkWidget *tl = label(t, low ? "lpt-warn" : "lpt-num", 1);
        gtk_box_append(GTK_BOX(top), tl);
        g_free(t);
        g_free(av);
        g_free(sz);
        gtk_box_append(GTK_BOX(card), top);
        LptBars *m = bars_new(1, TRUE);
        m->colors[0] = low ? C_AMBER : C_LINE_A;
        lp_spring_jump(&m->s[0], f->size ? 1.0 - (double)f->avail / (double)f->size : 0);
        gtk_box_append(GTK_BOX(card), GTK_WIDGET(m));
        if (low) {
            GtkWidget *w = label(T("Less than 15% left. Empty the trash or move big files "
                                   "to another drive before it fills up.",
                                   "15% 미만 남았습니다. 가득 차기 전에 휴지통을 비우거나 "
                                   "큰 파일을 다른 드라이브로 옮기세요."), "lpt-warn", 0);
            gtk_label_set_wrap(GTK_LABEL(w), TRUE);
            gtk_box_append(GTK_BOX(card), w);
        }
        gtk_box_append(GTK_BOX(A->fs_box), card);
    }
}

/* ── Network ──────────────────────────────────────────────────────── */

static GtkWidget *build_network(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Network", "네트워크"),
        T("Everything this computer sends and receives", "이 컴퓨터가 주고받는 모든 것"), NULL));
    A->g_net = graph_new(&S.rx_h, &S.tx_h, FMT_RATE, T("Receive", "받기"), T("Send", "보내기"));
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_net));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_rx, T("Receiving", "받는 중")),
        stat_new(&A->st_tx, T("Sending", "보내는 중")),
        stat_new(&A->st_rx_tot, T("Received since start-up", "켠 뒤 받은 양")),
        stat_new(&A->st_tx_tot, T("Sent since start-up", "켠 뒤 보낸 양"))));
    A->net_wifi = label("", "lpt-note", 0);
    gtk_widget_set_margin_top(A->net_wifi, 12);
    gtk_box_append(GTK_BOX(b), A->net_wifi);
    gtk_box_append(GTK_BOX(b), section(T("Connections", "연결")));
    A->net_ifaces = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(b), A->net_ifaces);
    return lp_kit_scroller(b, FALSE);
}

static void update_ifaces(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->net_ifaces)))
        gtk_box_remove(GTK_BOX(A->net_ifaces), c);
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || !strcmp(i->ifa_name, "lo"))
            continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;
        char addr[64] = "";
        if (fam == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof addr);
        else
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, addr, sizeof addr);
        kv_row(A->net_ifaces, i->ifa_name, addr);
    }
    freeifaddrs(ifa);
    if (!gtk_widget_get_first_child(A->net_ifaces))
        kv_row(A->net_ifaces, T("No connection", "연결 없음"), "");
}

/* lp-net status --json (the networking track's contract) for the Wi-Fi
 * line; the JSON is small and flat enough for two lookups by hand. */
static char *json_str(const char *j, const char *key)
{
    char *k = g_strdup_printf("\"%s\":\"", key);
    const char *p = strstr(j, k);
    g_free(k);
    if (!p)
        return NULL;
    p = strchr(p, ':') + 2;
    const char *e = strchr(p, '"');
    return e ? g_strndup(p, (gsize)(e - p)) : NULL;
}

static int json_int(const char *j, const char *key, int dflt)
{
    char *k = g_strdup_printf("\"%s\":", key);
    const char *p = strstr(j, k);
    g_free(k);
    return p ? atoi(strchr(p, ':') + 1) : dflt;
}

static void on_lpnet(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) || !out) {
        g_free(out);
        return;
    }
    char *state = json_str(out, "state"), *ssid = json_str(out, "ssid");
    int q = json_int(out, "quality", -1);
    char *t;
    if (state && !strcmp(state, "connected") && ssid && *ssid)
        t = g_strdup_printf(T("Wi-Fi: %s, signal %d%%", "Wi-Fi: %s, 신호 %d%%"), ssid, q);
    else
        t = g_strdup(T("Wi-Fi: not connected", "Wi-Fi: 연결 안 됨"));
    gtk_label_set_text(GTK_LABEL(A->net_wifi), t);
    g_free(t);
    g_free(state);
    g_free(ssid);
    g_free(out);
}

static void query_lpnet(void)
{
    char *p = g_find_program_in_path("lp-net");
    if (!p)
        return;
    const char *argv[] = { p, "status", "--json", NULL };
    GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (sp) {
        g_subprocess_communicate_utf8_async(sp, NULL, NULL, on_lpnet, NULL);
        g_object_unref(sp);
    }
    g_free(p);
}

/* ── Battery ──────────────────────────────────────────────────────── */

static GtkWidget *build_battery(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Battery", "배터리"), NULL, &A->bat_state));
    A->g_bat = graph_new(&A->bat_series, NULL, FMT_PERCENT, NULL, NULL);
    A->g_bat->caption = g_strdup(T("Since this window opened, up to 3 hours",
                                   "이 창을 연 뒤부터, 최대 3시간"));
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_bat));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_bat_pct, T("Charge", "잔량")),
        stat_new(&A->st_bat_time, T("Time left", "남은 시간")),
        stat_new(&A->st_bat_w, T("Using now", "현재 소비")),
        stat_new(&A->st_bat_health, T("Health", "수명"))));
    A->bat_note = label("", "lpt-hint", 0);
    gtk_label_set_wrap(GTK_LABEL(A->bat_note), TRUE);
    gtk_widget_set_halign(A->bat_note, GTK_ALIGN_START);
    gtk_widget_set_margin_top(A->bat_note, 16);
    gtk_box_append(GTK_BOX(b), A->bat_note);
    return lp_kit_scroller(b, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════
 * The once-a-second update
 * ═══════════════════════════════════════════════════════════════════ */

static void update_pages(void)
{
    char t[128];
    /* CPU */
    char *det;
    if (!isnan(S.temp))
        det = g_strdup_printf("%.2f GHz · %.0f °C", S.mhz / 1000, S.temp);
    else
        det = S.mhz > 0 ? g_strdup_printf("%.2f GHz", S.mhz / 1000) : g_strdup("");
    graph_sampled(A->g_cpu, det);
    g_free(det);
    for (int i = 0; i < S.ncpu; i++)
        bars_set(A->cores, i, S.core[i]);
    bars_kick(A->cores);
    g_snprintf(t, sizeof t, "%d", S.nprocs);
    stat_set(&A->st_procs, t, "", FALSE);
    g_snprintf(t, sizeof t, "%d", S.nthreads);
    stat_set(&A->st_threads, t, "", FALSE);
    char *up = fmt_duration(S.uptime);
    stat_set(&A->st_uptime, up, "", FALSE);
    g_free(up);
    if (!isnan(S.temp)) {
        g_snprintf(t, sizeof t, "%.0f", S.temp);
        stat_set(&A->st_temp, t, "°C", S.temp >= 85);
    } else {
        stat_set(&A->st_temp, "—", T("no sensor", "센서 없음"), FALSE);
    }

    /* Memory */
    guint64 used = S.mem_total - S.mem_avail;
    char *u = fmt_bytes((double)used), *tot = fmt_bytes((double)S.mem_total);
    char *md = g_strdup_printf(T("%s of %s", "%s / %s"), u, tot);
    graph_sampled(A->g_mem, md);
    g_free(md);
    g_free(u);
    g_free(tot);
    if (S.mem_total) {
        guint64 inuse = S.mem_total - S.mem_free - S.mem_cache;
        bars_set(A->mem_bar, 0, (double)MIN(inuse, S.mem_total) / (double)S.mem_total);
        bars_set(A->mem_bar, 1, (double)S.mem_cache / (double)S.mem_total);
        bars_kick(A->mem_bar);
    }
    stat_bytes(&A->st_mem_used, (double)used, FALSE);
    stat_bytes(&A->st_mem_avail, (double)S.mem_avail, FALSE);
    stat_bytes(&A->st_mem_cache, (double)S.mem_cache, FALSE);
    if (S.swap_total)
        stat_bytes(&A->st_swap, (double)(S.swap_total - S.swap_free), FALSE);
    else
        stat_set(&A->st_swap, "—", T("no swap", "스왑 없음"), FALSE);

    /* GPU */
    if (S.gpu_ok) {
        g_snprintf(t, sizeof t, "%d MHz", S.gpu_mhz);
        graph_sampled(A->g_gpu, t);
    } else {
        graph_sampled(A->g_gpu, T("no load reading", "부하 정보 없음"));
    }
    g_snprintf(t, sizeof t, "%d", S.gpu_mhz);
    stat_set(&A->st_gpu_freq, S.gpu_mhz ? t : "—", S.gpu_mhz ? "MHz" : "", FALSE);
    g_snprintf(t, sizeof t, "%d", S.gpu_max);
    stat_set(&A->st_gpu_max, S.gpu_max ? t : "—", S.gpu_max ? "MHz" : "", FALSE);
    stat_set(&A->st_gpu_drv, S.gpu_driver ? S.gpu_driver : "—", "", FALSE);
    GString *gn = g_string_new(S.gpu_ok
        ? T("Load is how much of the time the GPU was not in its sleep state (RC6).",
            "부하는 GPU가 절전 상태(RC6)가 아니었던 시간의 비율입니다.")
        : T("This GPU does not report how busy it is.", "이 GPU는 얼마나 바쁜지 알려 주지 않습니다."));
    if (S.dgpu) {
        g_string_append(gn, "\n");
        g_string_append(gn, S.dgpu);
    }
    gtk_label_set_text(GTK_LABEL(A->gpu_note), gn->str);
    g_string_free(gn, TRUE);

    /* Drives */
    graph_sampled(A->g_disk, NULL);
    char *wr = fmt_rate(S.wr);
    char *dd = g_strdup_printf(T("write %s", "쓰기 %s"), wr);
    g_free(A->g_disk->detail);
    A->g_disk->detail = dd;
    g_free(wr);
    stat_bytes(&A->st_rd, S.rd, TRUE);
    stat_bytes(&A->st_wr, S.wr, TRUE);

    /* Network */
    graph_sampled(A->g_net, NULL);
    char *tx = fmt_rate(S.tx);
    g_free(A->g_net->detail);
    A->g_net->detail = g_strdup_printf(T("send %s", "보내기 %s"), tx);
    g_free(tx);
    stat_bytes(&A->st_rx, S.rx, TRUE);
    stat_bytes(&A->st_tx, S.tx, TRUE);
    stat_bytes(&A->st_rx_tot, (double)S.rx_total, FALSE);
    stat_bytes(&A->st_tx_tot, (double)S.tx_total, FALSE);

    /* Battery */
    if (!S.bat) {
        gtk_label_set_text(GTK_LABEL(A->bat_state), T("This computer has no battery.",
                                                      "이 컴퓨터에는 배터리가 없습니다."));
        graph_sampled(A->g_bat, "");
        gtk_widget_set_visible(A->bat_note, FALSE);
    } else {
        const char *st = S.bat_state ? S.bat_state : "";
        const char *sttext = !strcmp(st, "Charging") ? T("Charging", "충전 중")
                           : !strcmp(st, "Discharging") ? T("On battery", "배터리 사용 중")
                           : !strcmp(st, "Full") ? T("Full", "완충")
                           : T("Plugged in", "전원 연결됨");
        gtk_label_set_text(GTK_LABEL(A->bat_state), sttext);
        graph_sampled(A->g_bat, sttext);
        g_snprintf(t, sizeof t, "%d", S.bat_pct);
        stat_set(&A->st_bat_pct, t, "%", S.bat_pct <= 15);
        if (S.bat_min >= 0) {
            char *d = fmt_duration(S.bat_min * 60.0);
            stat_set(&A->st_bat_time, d, "", FALSE);
            g_free(d);
        } else {
            stat_set(&A->st_bat_time, "—", "", FALSE);
        }
        if (S.bat_w >= 0) {
            g_snprintf(t, sizeof t, "%.1f", S.bat_w);
            stat_set(&A->st_bat_w, t, "W", FALSE);
        } else {
            stat_set(&A->st_bat_w, "—", "", FALSE);
        }
        if (S.bat_health >= 0) {
            g_snprintf(t, sizeof t, "%.0f", S.bat_health);
            char *cy = S.bat_cycles >= 0 ? g_strdup_printf(T("%% · %d cycles", "%% · %d회 충전"),
                                                          S.bat_cycles) : g_strdup("%");
            stat_set(&A->st_bat_health, t, cy, S.bat_health < 70);
            g_free(cy);
        } else {
            stat_set(&A->st_bat_health, "—", "", FALSE);
        }
        /* The sentence the number needs. */
        if (!strcmp(st, "Discharging") && S.bat_min >= 0 && S.bat_w > 0) {
            char *d = fmt_duration(S.bat_min * 60.0);
            char *h = g_strdup_printf(T("At %.1f W the battery lasts about %s more. Lower "
                                        "brightness is the biggest saving on this screen.",
                                        "%.1f W 로 쓰면 약 %s 더 쓸 수 있습니다. 이 화면에서는 "
                                        "밝기를 낮추는 것이 가장 크게 아낍니다."), S.bat_w, d);
            gtk_label_set_text(GTK_LABEL(A->bat_note), h);
            gtk_widget_set_visible(A->bat_note, TRUE);
            g_free(h);
            g_free(d);
        } else {
            gtk_widget_set_visible(A->bat_note, FALSE);
        }
    }
}

static gboolean sample_tick(gpointer d)
{
    (void)d;
    gint64 now = g_get_monotonic_time();
    double secs = A->last_sample ? (now - A->last_sample) / 1e6 : 1.0;
    A->last_sample = now;
    S.now = now;
    sample_cpu();
    sample_mem();
    sample_disk(secs);
    sample_net(secs);
    sample_gpu();
    S.temp = temp_path ? read_num(temp_path, -1000000) / 1000.0 : NAN;
    if (S.temp < -100)
        S.temp = NAN;
    sample_battery();
    if (S.primed) {
        series_push(&S.cpu_h, now, S.cpu);
        series_push(&S.mem_h, now, S.mem_total ?
                    100.0 * (double)(S.mem_total - S.mem_avail) / (double)S.mem_total : 0);
        series_push(&S.rd_h, now, S.rd);
        series_push(&S.wr_h, now, S.wr);
        series_push(&S.rx_h, now, S.rx);
        series_push(&S.tx_h, now, S.tx);
        series_push(&S.gpu_h, now, S.gpu);
        if (S.bat)
            series_push(&A->bat_series, now, S.bat_pct);
    }
    S.primed = TRUE;

    /* The walk over /proc only when someone can see its result. */
    gboolean visible = !window_minimised();
    if (visible && (page_is("apps") || page_is("processes") || page_is("memory"))) {
        scan_procs();
        if (page_is("apps"))
            update_apps();
        if (page_is("memory"))
            update_mem_top();
        if (page_is("processes")) {
            gtk_sorter_changed(A->proc_sorter, GTK_SORTER_CHANGE_DIFFERENT);
            char t[96];
            g_snprintf(t, sizeof t, T("%d processes, %d threads", "프로세스 %d개, 스레드 %d개"),
                       S.nprocs, S.nthreads);
            gtk_label_set_text(GTK_LABEL(A->proc_count), t);
        }
    } else if (visible && page_is("cpu")) {
        count_procs();
    }
    if (visible && ++A->slow_count % 5 == 1) {
        if (page_is("drives"))
            update_fs();
        if (page_is("network")) {
            update_ifaces();
            query_lpnet();
        }
        if (page_is("gpu"))
            sample_dgpu();
    }
    if (visible)
        update_pages();
    return G_SOURCE_CONTINUE;
}

/* ── the sidebar ──────────────────────────────────────────────────── */

static const struct { const char *id, *icon, *en, *ko; gboolean group_before; } PAGES[] = {
    { "apps",      "view-app-grid-symbolic",      "Apps",      "앱", FALSE },
    { "processes", "view-list-symbolic",          "Processes", "프로세스", FALSE },
    { "cpu",       "computer-symbolic",           "CPU",       "CPU", TRUE },
    { "memory",    "media-flash-symbolic",        "Memory",    "메모리", FALSE },
    { "gpu",       "video-display-symbolic",      "GPU",       "GPU", FALSE },
    { "drives",    "drive-harddisk-symbolic",     "Drives",    "드라이브", FALSE },
    { "network",   "network-wired-symbolic",      "Network",   "네트워크", FALSE },
    { "battery",   "battery-full-symbolic",       "Battery",   "배터리", FALSE },
};

static void go_page(const char *id)
{
    g_free(A->page);
    A->page = g_strdup(id);
    lp_kit_stack_show(GTK_STACK(A->stack), id);
    g_key_file_set_string(A->state, "tasks", "page", id);
    lp_kit_state_save(A->state, STATE_NAME);
    /* A page just arrived at should not wait a second for its numbers. */
    sample_tick(NULL);
}

static void on_side(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (!row)
        return;
    const char *id = g_object_get_data(G_OBJECT(row), "page");
    if (id)
        go_page(id);
}

static GtkWidget *build_sidebar(void)
{
    GtkWidget *lb = gtk_list_box_new();
    gtk_widget_add_css_class(lb, "lp-kit-side");
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++) {
        if (PAGES[i].group_before) {
            GtkWidget *g = label(T("Resources", "자원"), "lp-kit-group", 0);
            gtk_widget_set_margin_top(g, 14);
            gtk_widget_set_margin_start(g, 10);
            gtk_widget_set_margin_bottom(g, 4);
            gtk_list_box_append(GTK_LIST_BOX(lb), g);
            GtkListBoxRow *gr = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb),
                (int)i + (i > 0 && PAGES[0].group_before ? 1 : 0));
            GtkWidget *last = GTK_WIDGET(gr);
            (void)last;
            GtkWidget *row = gtk_widget_get_parent(g);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_widget_add_css_class(row, "lp-kit-group-row");
        }
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(PAGES[i].icon));
        gtk_box_append(GTK_BOX(box), label(T(PAGES[i].en, PAGES[i].ko), NULL, 0));
        gtk_list_box_append(GTK_LIST_BOX(lb), box);
        GtkWidget *row = gtk_widget_get_parent(box);
        g_object_set_data(G_OBJECT(row), "page", (gpointer)PAGES[i].id);
        char *nm = g_strdup_printf("side-%s", PAGES[i].id);
        gtk_widget_set_name(row, nm);
        g_free(nm);
    }
    g_signal_connect(lb, "row-activated", G_CALLBACK(on_side), NULL);
    GtkWidget *sc = lp_kit_scroller(lb, FALSE);
    gtk_widget_add_css_class(sc, "lp-kit-sidebar");
    gtk_widget_set_size_request(sc, SIDEBAR_W, -1);
    gtk_widget_set_hexpand(sc, FALSE);
    A->sidebar = lb;
    return sc;
}

static void select_side(const char *id)
{
    for (GtkWidget *c = gtk_widget_get_first_child(A->sidebar); c;
         c = gtk_widget_get_next_sibling(c)) {
        const char *p = g_object_get_data(G_OBJECT(c), "page");
        if (p && !strcmp(p, id))
            gtk_list_box_select_row(GTK_LIST_BOX(A->sidebar), GTK_LIST_BOX_ROW(c));
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Start
 * ═══════════════════════════════════════════════════════════════════ */

static const char APP_CSS[] =
    ".lpt-main { background-color: #0e0e0e; }\n"
    ".lpt-page { padding: 20px 26px 22px 26px; }\n"
    ".lpt-h1 { font-size: 20px; font-weight: 600; letter-spacing: -0.02em; }\n"
    ".lpt-sub { font-size: 13px; color: #6a6a6a; }\n"
    ".lpt-sec { font-size: 12px; color: #6a6a6a; }\n"
    "lptgraph { background-color: #232323; border-radius: 10px; min-height: 230px; }\n"
    ".lpt-stat { background-color: #232323; border-radius: 8px; padding: 11px 13px; }\n"
    ".lpt-stat-k { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-stat-v { font-size: 20px; font-weight: 600; font-feature-settings: 'tnum'; }\n"
    ".lpt-stat-u { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-stat.warn .lpt-stat-v { color: #f0b350; }\n"
    ".lpt-kv { padding: 7px 0; border-bottom: 0.5px solid #1e1e1e; }\n"
    ".lpt-kv-k { font-size: 13px; color: #a8a8a8; }\n"
    ".lpt-kv-v { font-size: 13px; color: #e8e8e8; font-feature-settings: 'tnum'; }\n"
    ".lpt-colhead { font-size: 12px; color: #6a6a6a; padding: 0 12px 6px 12px;"
    "  border-bottom: 0.5px solid #333333; }\n"
    ".lpt-list { background: none; }\n"
    ".lpt-list > row { padding: 0; background: none; }\n"
    ".lpt-app-row { padding: 6px 12px; min-height: 48px; border-bottom: 0.5px solid #262626; }\n"
    ".lpt-app-name { font-size: 14px; font-weight: 500; }\n"
    ".lpt-dim { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-num { font-size: 13px; font-feature-settings: 'tnum'; }\n"
    ".lpt-num.hot { color: #f0b350; }\n"
    ".lpt-warn { font-size: 13px; color: #f0b350; }\n"
    ".lpt-note { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-hint { background-color: #2a2113; color: #f0b350; border: 0.5px solid #4a3510;"
    "  border-radius: 7px; padding: 9px 13px; font-size: 13px; }\n"
    ".lpt-action { min-height: 40px; min-width: 88px; border-radius: 8px; }\n"
    ".lpt-search { min-height: 40px; border-radius: 8px; }\n"
    ".lpt-procs { background: none; }\n"
    ".lpt-procs listview row { min-height: 40px; }\n"
    ".lpt-procs listview row cell { padding: 0 10px; }\n"
    ".lpt-cell-name { font-size: 13px; }\n"
    ".lpt-fs { background-color: #232323; border-radius: 8px; padding: 12px 14px; }\n"
    ".lpt-dot { min-width: 10px; min-height: 10px; border-radius: 5px; }\n"
    ".lpt-dot.a { background-color: #e8e8e8; }\n"
    ".lpt-dot.b { background-color: rgba(127,184,160,0.8); }\n"
    ".lpt-dot.c { background-color: rgba(255,255,255,0.12); }\n"
    ".lp-kit-group-row { min-height: 0; }\n";

static void on_drive(const char *verb, const char *arg, gpointer d)
{
    (void)d;
    if (!strcmp(verb, "page"))
        go_page(arg), select_side(arg);
}

static char *opt_page;

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (A->win) {
        gtk_window_present(GTK_WINDOW(A->win));
        return;
    }
    lp_kit_style(APP_CSS);
    A->gapp = app;
    A->win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(A->win), T("Task Manager", "작업 관리자"));
    gtk_window_set_default_size(GTK_WINDOW(A->win), WINDOW_W, WINDOW_H);
    gtk_window_set_icon_name(GTK_WINDOW(A->win), "utilities-system-monitor");

    static const GActionEntry acts[] = {
        { "proc-end", act_proc, NULL, NULL, NULL, { 0 } },
        { "proc-kill", act_proc, NULL, NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(A->win), acts, 1, GINT_TO_POINTER(SIGTERM));
    g_action_map_add_action_entries(G_ACTION_MAP(A->win), acts + 1, 1, GINT_TO_POINTER(SIGKILL));

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(outer), build_sidebar());
    A->stack = lp_kit_stack();
    gtk_widget_add_css_class(A->stack, "lpt-main");
    gtk_widget_set_hexpand(A->stack, TRUE);
    gtk_widget_set_vexpand(A->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(A->stack), build_apps(), "apps");
    gtk_stack_add_named(GTK_STACK(A->stack), build_processes(), "processes");
    gtk_stack_add_named(GTK_STACK(A->stack), build_cpu(), "cpu");
    gtk_stack_add_named(GTK_STACK(A->stack), build_memory(), "memory");
    gtk_stack_add_named(GTK_STACK(A->stack), build_gpu(), "gpu");
    gtk_stack_add_named(GTK_STACK(A->stack), build_drives(), "drives");
    gtk_stack_add_named(GTK_STACK(A->stack), build_network(), "network");
    gtk_stack_add_named(GTK_STACK(A->stack), build_battery(), "battery");
    gtk_box_append(GTK_BOX(outer), A->stack);
    gtk_window_set_child(GTK_WINDOW(A->win), outer);

    char *saved = g_key_file_get_string(A->state, "tasks", "page", NULL);
    const char *page = opt_page ? opt_page : saved ? saved : "apps";
    gboolean known = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++)
        known |= !strcmp(PAGES[i].id, page);
    if (!known)
        page = "apps";
    A->page = g_strdup(page);
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 0);
    gtk_stack_set_visible_child_name(GTK_STACK(A->stack), page);
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 260);
    select_side(page);
    g_free(saved);

    gtk_window_present(GTK_WINDOW(A->win));
    lp_kit_drive(on_drive, NULL);
    sample_tick(NULL);
    g_timeout_add_seconds(1, sample_tick, NULL);
}

int main(int argc, char **argv)
{
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--page="))
            opt_page = g_strdup(argv[i] + 7);
        else
            argv[out++] = argv[i];
    }
    argc = out;
    argv[argc] = NULL;

    page_size = sysconf(_SC_PAGESIZE);
    clk_tck = sysconf(_SC_CLK_TCK);
    A = g_new0(App, 1);
    A->state = lp_kit_state_load(STATE_NAME);
    A->app_rows = g_hash_table_new(g_str_hash, g_str_equal);
    procs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
    proc_store = g_list_store_new(LPT_TYPE_PROC);
    users = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    S.temp = NAN;
    read_cpuinfo();
    find_temp();
    find_gpu();
    find_battery();
    load_exe_apps();

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
