/* lp-tune - power profiles, the battery, and the backlight.
 *
 *   lp-tune                          what is in force, and why
 *   lp-tune status [--json]          the same, as JSON for the top bar
 *   lp-tune set auto|balanced|saver|performance
 *   lp-tune brightness get           0-100
 *   lp-tune brightness set N|+N|-N   N percent, or a step up or down
 *   lp-tune -d                       the daemon, run from /etc/services
 *
 *   --sysfs DIR   read and write under DIR instead of /  (tests)
 *   --dry-run     print every write instead of doing it
 *
 * ── Why this exists ──
 * A laptop left to the kernel's defaults runs every device at full power
 * for ever. None of it is wrong - the defaults are chosen so that no
 * machine anywhere misbehaves - but on a Dell XPS 15 9550 the price is
 * an hour or more of battery. The GTX 960M alone is the worst of it: on
 * an Optimus laptop the screen is wired to the Intel GPU, the NVIDIA one
 * is idle nearly always, and it only switches off when its PCI device is
 * allowed to runtime-suspend. Nothing allows that by default.
 *
 * So this is the whole of what a distribution normally spreads over
 * power-profiles-daemon, TLP, udev rules and a brightness helper, in one
 * place that can be read top to bottom:
 *
 *   CPU        intel_pstate's energy/performance preference, and turbo
 *   PCIe       link power management (ASPM), and runtime PM for every
 *              device - which is what powers the dGPU off
 *   USB        autosuspend for everything except input devices
 *   SATA       link power management, for the models that have SATA
 *   audio      the HDA codec's power save
 *   kernel     the NMI watchdog, and how long dirty pages wait
 *   backlight  the one place brightness is written
 *
 * ── Profiles ──
 * Three that do something and one that chooses between them:
 *
 *   saver        on battery. Slower, quieter, much longer.
 *   balanced     plugged in. Everything power-managed that costs nothing
 *                you would notice, turbo on.
 *   performance  when asked. Turbo, no ASPM, no audio power save.
 *   auto         balanced on AC, saver on battery, switched the moment
 *                the charger goes in or comes out. The default.
 *
 * Every profile keeps PCI runtime PM on. "Performance" is about the CPU;
 * the NVIDIA GPU is not drawing the screen in any of them, and letting
 * it stay powered would be spending battery on nothing.
 *
 * ── What is never power-managed ──
 * Input devices. USB autosuspend on a touch controller adds the resume
 * time to the first touch after a pause - a finger on the glass and a
 * tenth of a second of nothing - and the owner's first requirement is
 * that touch is perfect. Any USB device with a HID interface stays on.
 *
 * ── Brightness ──
 * Writing /sys/class/backlight needs root, and the person dragging the
 * brightness slider is not root. Every desktop solves that somehow; this
 * machine solves it here: the daemon owns the backlight and anyone at the
 * machine may ask it for a new value. It never goes below 1% - a panel at
 * zero looks switched off, and on a touch laptop with no keyboard handy
 * there is then no way to find the slider again.
 *
 * ── Who may ask ──
 * The socket is /run/lp-tune.sock and the daemon asks the kernel who is
 * on the other end (SO_PEERCRED): root, or a real account (uid >= 1000),
 * which is the rule lp-power uses and the rule polkit arrives at for the
 * person at the console. A service account may not. Only four words are
 * accepted, and the only effect any of them can have is on power and
 * brightness, so this is a convenience boundary, not a security one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "syscall.h"

#define SOCK_PATH     "/run/lp-tune.sock"
#define PROFILE_FILE  "/etc/lp-tune.profile"
#define AF_UNIX_      1
#define AF_NETLINK_   16
#define NETLINK_KOBJECT_UEVENT_ 15
#define SO_PEERCRED_  17
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* ── Options that apply everywhere ─────────────────────────────────── */

static char root[256];        /* "" normally; a fake tree in tests */
static bool dry_run;

/* ── Small file helpers, all relative to `root` ────────────────────── */

static void rooted(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s%s", root, path);
}

/* Read a sysfs attribute, newline stripped. false if it is not there. */
static bool rd(const char *path, char *buf, size_t n)
{
    char p[512];
    rooted(path, p, sizeof p);
    long got = proc_read(p, buf, n);
    if (got <= 0)
        return false;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static long rd_num(const char *path, long dflt)
{
    char b[64];
    if (!rd(path, b, sizeof b))
        return dflt;
    return strtol(b, NULL, 10);
}

/* Write one value. Counts what it did, so the report can say "applied
 * 23 settings, 2 not present" rather than nothing at all. A missing
 * attribute is not an error - most of the table is for hardware a given
 * machine does not have. A present one that refuses the value is, and is
 * said once. */
static int n_written, n_absent, n_refused;

static void wr(const char *path, const char *val)
{
    char p[512];
    rooted(path, p, sizeof p);

    if (!lp_exists(p)) {
        n_absent++;
        return;
    }
    /* Do not rewrite a value that is already right: a sysfs write can
     * have side effects (a USB device resumed to re-read its policy),
     * and doing it every time the charger is touched is pointless. */
    char cur[128];
    if (proc_read(p, cur, sizeof cur) > 0) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = '\0';
        /* The ASPM policy file shows every choice with the current one
         * in brackets: "default performance [powersave] powersupersave" */
        char want[64];
        snprintf(want, sizeof want, "[%s]", val);
        if (strcmp(cur, val) == 0 || strstr(cur, want)) {
            n_written++;
            return;
        }
    }
    if (dry_run) {
        printf("  write %s = %s\n", path, val);
        n_written++;
        return;
    }
    long fd = lp_open(p, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        n_refused++;
        dprintf(STDERR_FILENO, "lp-tune: cannot open %s (%ld)\n", path, -fd);
        return;
    }
    long w = lp_write((int)fd, val, strlen(val));
    lp_close((int)fd);
    if (w < 0) {
        n_refused++;
        dprintf(STDERR_FILENO, "lp-tune: %s refused \"%s\" (%ld)\n",
                path, val, -w);
    } else {
        n_written++;
    }
}

/* Call fn(dir/name) for every entry of a directory, skipping dot files. */
static void each_entry(const char *dir, void (*fn)(const char *path, void *),
                       void *arg)
{
    char p[512];
    rooted(dir, p, sizeof p);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;
    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            u16 len;
            memcpy(&len, buf + off + DIRENT_RECLEN, sizeof len);
            const char *name = buf + off + DIRENT_NAME;
            off += len;
            if (name[0] == '.')
                continue;
            char full[512];
            snprintf(full, sizeof full, "%s/%s", dir, name);
            fn(full, arg);
        }
    }
    lp_close((int)fd);
}

/* ── Power supply ──────────────────────────────────────────────────── */

typedef struct {
    bool have_ac, ac;
    bool have_bat;
    int  percent;
    char state[16];         /* charging | discharging | full | unknown */
    long minutes_left;      /* -1 when it cannot be worked out */
} power_t;

static void ps_one(const char *path, void *arg)
{
    power_t *p = arg;
    char f[512], type[32];
    snprintf(f, sizeof f, "%s/type", path);
    if (!rd(f, type, sizeof type))
        return;

    if (strcmp(type, "Mains") == 0) {
        snprintf(f, sizeof f, "%s/online", path);
        p->have_ac = true;
        if (rd_num(f, 0) == 1)
            p->ac = true;
        return;
    }
    if (strcmp(type, "Battery") != 0)
        return;

    /* A peripheral's battery (a wireless mouse) also shows up here, with
     * scope=Device. It is not the machine's battery. */
    char scope[16];
    snprintf(f, sizeof f, "%s/scope", path);
    if (rd(f, scope, sizeof scope) && strcmp(scope, "Device") == 0)
        return;
    snprintf(f, sizeof f, "%s/present", path);
    if (rd_num(f, 1) != 1)
        return;

    p->have_bat = true;
    snprintf(f, sizeof f, "%s/capacity", path);
    p->percent = (int)rd_num(f, -1);

    char st[32] = "unknown";
    snprintf(f, sizeof f, "%s/status", path);
    rd(f, st, sizeof st);
    if      (strcmp(st, "Charging") == 0)    strlcpy(p->state, "charging", sizeof p->state);
    else if (strcmp(st, "Discharging") == 0) strlcpy(p->state, "discharging", sizeof p->state);
    else if (strcmp(st, "Full") == 0)        strlcpy(p->state, "full", sizeof p->state);
    else                                     strlcpy(p->state, "unknown", sizeof p->state);

    /* Time left: batteries report either energy (uWh, with power in uW)
     * or charge (uAh, with current in uA). Dell's report energy. */
    long now  = -1, full = -1, rate = -1;
    snprintf(f, sizeof f, "%s/energy_now", path);  now  = rd_num(f, -1);
    snprintf(f, sizeof f, "%s/energy_full", path); full = rd_num(f, -1);
    snprintf(f, sizeof f, "%s/power_now", path);   rate = rd_num(f, -1);
    if (now < 0) {
        snprintf(f, sizeof f, "%s/charge_now", path);  now  = rd_num(f, -1);
        snprintf(f, sizeof f, "%s/charge_full", path); full = rd_num(f, -1);
        snprintf(f, sizeof f, "%s/current_now", path); rate = rd_num(f, -1);
    }
    p->minutes_left = -1;
    if (rate > 0 && now >= 0) {
        if (strcmp(p->state, "discharging") == 0)
            p->minutes_left = now * 60 / rate;
        else if (strcmp(p->state, "charging") == 0 && full > now)
            p->minutes_left = (full - now) * 60 / rate;
    }
}

static void read_power(power_t *p)
{
    memset(p, 0, sizeof *p);
    p->percent = -1;
    p->minutes_left = -1;
    strlcpy(p->state, "unknown", sizeof p->state);
    each_entry("/sys/class/power_supply", ps_one, p);
    /* A desktop, a VM, or a laptop whose AC adapter the firmware does
     * not describe: with no Mains entry at all, assume the wall. Guessing
     * battery would put a desktop into power-saver for ever. */
    if (!p->have_ac)
        p->ac = !p->have_bat || strcmp(p->state, "discharging") != 0;
}

/* ── Backlight ─────────────────────────────────────────────────────── */

/* Which backlight device drives the panel. The kernel can register
 * several for one screen - one from ACPI ("firmware"), one from a
 * platform driver, and the GPU's own ("raw", intel_backlight on this
 * laptop) - and writing the wrong one does nothing visible. The order
 * here is systemd's, which has had a decade of laptops to learn from:
 * firmware, then platform, then raw. */
typedef struct { char name[64]; int rank; } bl_pick_t;

static void bl_one(const char *path, void *arg)
{
    bl_pick_t *b = arg;
    char f[512], type[32];
    snprintf(f, sizeof f, "%s/type", path);
    if (!rd(f, type, sizeof type))
        return;
    snprintf(f, sizeof f, "%s/max_brightness", path);
    if (rd_num(f, 0) <= 1)          /* an on/off switch, not a dimmer */
        return;
    int rank = strcmp(type, "firmware") == 0 ? 3
             : strcmp(type, "platform") == 0 ? 2
             : strcmp(type, "raw") == 0      ? 1 : 0;
    if (rank > b->rank) {
        const char *slash = strrchr(path, '/');
        strlcpy(b->name, slash ? slash + 1 : path, sizeof b->name);
        b->rank = rank;
    }
}

static bool backlight(char *name, size_t n)
{
    bl_pick_t b;
    memset(&b, 0, sizeof b);
    each_entry("/sys/class/backlight", bl_one, &b);
    if (!b.name[0])
        return false;
    strlcpy(name, b.name, n);
    return true;
}

static int brightness_get(void)
{
    char dev[64], f[256];
    if (!backlight(dev, sizeof dev))
        return -1;
    snprintf(f, sizeof f, "/sys/class/backlight/%s/max_brightness", dev);
    long max = rd_num(f, 0);
    snprintf(f, sizeof f, "/sys/class/backlight/%s/brightness", dev);
    long cur = rd_num(f, -1);
    if (max <= 0 || cur < 0)
        return -1;
    return (int)((cur * 100 + max / 2) / max);
}

static bool brightness_set(int pct)
{
    char dev[64], f[256];
    if (!backlight(dev, sizeof dev))
        return false;
    if (pct < 1)   pct = 1;          /* never black - see the header */
    if (pct > 100) pct = 100;
    snprintf(f, sizeof f, "/sys/class/backlight/%s/max_brightness", dev);
    long max = rd_num(f, 0);
    if (max <= 0)
        return false;
    long raw = max * pct / 100;
    if (raw < 1) raw = 1;
    char v[32];
    snprintf(v, sizeof v, "%ld", raw);
    snprintf(f, sizeof f, "/sys/class/backlight/%s/brightness", dev);
    int before = n_refused;
    wr(f, v);
    return n_refused == before;
}

/* ── Profiles ──────────────────────────────────────────────────────── */

typedef enum { P_AUTO, P_BALANCED, P_SAVER, P_PERFORMANCE } profile_t;
static const char *PNAME[] = { "auto", "balanced", "saver", "performance" };

static bool parse_profile(const char *s, profile_t *out)
{
    for (int i = 0; i < 4; i++)
        if (strcmp(s, PNAME[i]) == 0) { *out = (profile_t)i; return true; }
    return false;
}

static profile_t load_profile(void)
{
    char b[32];
    profile_t p = P_AUTO;
    if (rd(PROFILE_FILE, b, sizeof b))
        parse_profile(b, &p);
    return p;
}

static void save_profile(profile_t p)
{
    if (dry_run)
        return;
    char path[512], tmp[520];
    rooted(PROFILE_FILE, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    dprintf((int)fd, "%s\n", PNAME[p]);
    lp_close((int)fd);
    lp_rename(tmp, path);
}

static profile_t effective(profile_t p, const power_t *pw)
{
    if (p != P_AUTO)
        return p;
    return pw->ac ? P_BALANCED : P_SAVER;
}

/* Per-CPU: the energy/performance preference and nothing else. The
 * governor stays whatever intel_pstate chose (powersave, which on
 * intel_pstate means "let the hardware decide", not "slow"). */
static const char *cur_epp;
static void cpu_one(const char *path, void *arg)
{
    (void)arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strncmp(name, "cpu", 3) != 0 || name[3] < '0' || name[3] > '9')
        return;
    char f[512];
    snprintf(f, sizeof f, "%s/cpufreq/energy_performance_preference", path);
    wr(f, cur_epp);
}

static const char *cur_pci_pm;
static void pci_one(const char *path, void *arg)
{
    (void)arg;
    char f[512];
    snprintf(f, sizeof f, "%s/power/control", path);
    wr(f, cur_pci_pm);
}

/* True when a USB device has any HID interface - keyboards, mice,
 * touchscreens, touchpads, pens. Those are never autosuspended. */
typedef struct { const char *dev; bool hid; } hid_scan_t;
static void usb_if_one(const char *path, void *arg)
{
    hid_scan_t *h = arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t dl = strlen(h->dev);
    /* interfaces of device 1-2 are named 1-2:1.0, 1-2:1.1 ... */
    if (strncmp(name, h->dev, dl) != 0 || name[dl] != ':')
        return;
    char f[512], cls[8];
    snprintf(f, sizeof f, "%s/bInterfaceClass", path);
    if (rd(f, cls, sizeof cls) && strcmp(cls, "03") == 0)
        h->hid = true;
}

static const char *cur_usb;
static void usb_one(const char *path, void *arg)
{
    (void)arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strchr(name, ':'))          /* an interface, not a device */
        return;
    hid_scan_t h = { name, false };
    each_entry("/sys/bus/usb/devices", usb_if_one, &h);
    char f[512];
    snprintf(f, sizeof f, "%s/power/control", path);
    wr(f, h.hid ? "on" : cur_usb);
}

static const char *cur_alpm;
static void scsi_one(const char *path, void *arg)
{
    (void)arg;
    char f[512];
    snprintf(f, sizeof f, "%s/link_power_management_policy", path);
    wr(f, cur_alpm);
}

static void apply(profile_t eff)
{
    n_written = n_absent = n_refused = 0;

    bool saver = eff == P_SAVER, perf = eff == P_PERFORMANCE;

    cur_epp = saver ? "power" : perf ? "performance" : "balance_performance";
    each_entry("/sys/devices/system/cpu", cpu_one, NULL);
    wr("/sys/devices/system/cpu/intel_pstate/no_turbo", saver ? "1" : "0");

    wr("/sys/module/pcie_aspm/parameters/policy",
       saver ? "powersupersave" : perf ? "default" : "powersave");

    cur_pci_pm = "auto";                    /* every profile - see header */
    each_entry("/sys/bus/pci/devices", pci_one, NULL);

    cur_usb = "auto";
    each_entry("/sys/bus/usb/devices", usb_one, NULL);

    cur_alpm = perf ? "max_performance" : "med_power_with_dipm";
    each_entry("/sys/class/scsi_host", scsi_one, NULL);

    wr("/sys/module/snd_hda_intel/parameters/power_save", perf ? "0" : "1");
    wr("/sys/module/snd_hda_intel/parameters/power_save_controller",
       perf ? "N" : "Y");

    /* The NMI watchdog wakes every CPU periodically to check for hard
     * lockups. A laptop has the hardware watchdog for that. */
    wr("/proc/sys/kernel/nmi_watchdog", "0");

    /* How long dirty pages may sit before the flusher writes them: long
     * on battery so the NVMe can stay in its deepest state. */
    wr("/proc/sys/vm/dirty_writeback_centisecs",
       saver ? "6000" : perf ? "500" : "1500");
    wr("/proc/sys/vm/laptop_mode", saver ? "5" : "0");
}

/* ── Reporting ─────────────────────────────────────────────────────── */

static void json_status(int fd, profile_t prof)
{
    power_t pw;
    read_power(&pw);
    profile_t eff = effective(prof, &pw);
    dprintf(fd, "{\"profile\":\"%s\",\"effective\":\"%s\",\"ac\":%s,"
                "\"battery\":{\"present\":%s,\"percent\":%d,\"state\":\"%s\","
                "\"minutes_left\":%ld},\"brightness\":%d}\n",
            PNAME[prof], PNAME[eff], pw.ac ? "true" : "false",
            pw.have_bat ? "true" : "false", pw.percent, pw.state,
            pw.minutes_left, brightness_get());
}

static void human_status(profile_t prof)
{
    power_t pw;
    read_power(&pw);
    profile_t eff = effective(prof, &pw);
    printf("\n  profile      %s", PNAME[prof]);
    if (prof == P_AUTO)
        printf("  (%s now - %s)", PNAME[eff],
               pw.ac ? "on the charger" : "on battery");
    printf("\n");
    if (pw.have_bat) {
        printf("  battery      %d%%  %s", pw.percent, pw.state);
        if (pw.minutes_left >= 0)
            printf(", %ldh %02ldm %s", pw.minutes_left / 60,
                   pw.minutes_left % 60,
                   strcmp(pw.state, "charging") == 0 ? "to full" : "left");
        printf("\n");
    } else {
        printf("  battery      none - this machine runs from the wall\n");
    }
    int b = brightness_get();
    if (b >= 0) printf("  brightness   %d%%\n", b);
    else        printf("  brightness   no backlight to control here\n");
    printf("\n");
}

/* ── The daemon's socket ───────────────────────────────────────────── */

typedef struct { u16 family; char path[108]; } sun_t;

/* Fill a sockaddr_un, or refuse. sun_path is 108 bytes and strlcpy would
 * cut a longer path short without a word - the daemon would then listen
 * on one name and a client look for another, and "the service is not
 * running" would be the only thing anyone ever saw. */
static bool sun_fill(sun_t *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->family = AF_UNIX_;
    if (strlen(path) >= sizeof sa->path) {
        dprintf(STDERR_FILENO, "lp-tune: socket path too long (%u bytes, the"
                " limit is %u): %s\n", (unsigned)strlen(path),
                (unsigned)sizeof sa->path - 1, path);
        return false;
    }
    strlcpy(sa->path, path, sizeof sa->path);
    return true;
}

static bool peer_uid(int fd, u32 *uid)
{
    u32 cred[3];            /* struct ucred { pid; uid; gid; } - 12 bytes on every arch */
    u32 len = sizeof cred;
    long r = sys_call5(SYS_getsockopt, fd, SOL_SOCKET, SO_PEERCRED_,
                       (long)cred, (long)&len);
    if (r < 0 || len < sizeof cred)
        return false;
    *uid = cred[1];
    return true;
}

/* Handle one request line. Returns the reply. `prof` is the daemon's
 * state and is updated in place. */
static void handle(const char *req, u32 uid, profile_t *prof, char *reply,
                   size_t n)
{
    if (uid != 0 && uid < 1000) {
        snprintf(reply, n, "err only a person at this machine may change this\n");
        return;
    }
    char word[16], arg[16];
    word[0] = arg[0] = '\0';
    int k = 0;
    while (*req == ' ') req++;
    while (*req && *req != ' ' && *req != '\n' && k < 15) word[k++] = *req++;
    word[k] = '\0';
    while (*req == ' ') req++;
    k = 0;
    while (*req && *req != ' ' && *req != '\n' && k < 15) arg[k++] = *req++;
    arg[k] = '\0';

    if (strcmp(word, "set") == 0) {
        profile_t p;
        if (!parse_profile(arg, &p)) {
            snprintf(reply, n, "err no profile called \"%s\"\n", arg);
            return;
        }
        *prof = p;
        save_profile(p);
        power_t pw;
        read_power(&pw);
        apply(effective(p, &pw));
        snprintf(reply, n, "ok %s\n", PNAME[p]);
        return;
    }
    if (strcmp(word, "brightness") == 0) {
        int cur = brightness_get(), want;
        if (cur < 0) {
            snprintf(reply, n, "err no backlight on this machine\n");
            return;
        }
        if (arg[0] == '+' || arg[0] == '-')
            want = cur + (int)strtol(arg, NULL, 10);
        else if (arg[0] >= '0' && arg[0] <= '9')
            want = (int)strtol(arg, NULL, 10);
        else {
            snprintf(reply, n, "err brightness wants a number\n");
            return;
        }
        if (!brightness_set(want)) {
            snprintf(reply, n, "err the backlight refused it\n");
            return;
        }
        snprintf(reply, n, "ok %d\n", brightness_get());
        return;
    }
    snprintf(reply, n, "err say set <profile> or brightness <n>\n");
}

static int daemon_main(void)
{
    profile_t prof = load_profile();
    power_t pw;
    read_power(&pw);
    bool last_ac = pw.ac;
    apply(effective(prof, &pw));
    printf("lp-tune: %s (%s) - %d settings applied, %d not on this machine%s\n",
           PNAME[prof], PNAME[effective(prof, &pw)], n_written, n_absent,
           n_refused ? ", some refused - see above" : "");

    lp_signal_ignore(13);                   /* SIGPIPE: a client that left */

    char sp[256];
    rooted(SOCK_PATH, sp, sizeof sp);
    lp_unlink(sp);
    sun_t sa;
    long ls = sun_fill(&sa, sp) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
    if (ls < 0 || lp_bind((int)ls, &sa, sizeof sa) < 0 ||
        lp_listen((int)ls, 8) < 0) {
        dprintf(STDERR_FILENO, "lp-tune: cannot listen on %s - brightness and"
                " profiles are root-only until this is fixed\n", SOCK_PATH);
        ls = -1;
    } else {
        lp_chmod(sp, 0666);     /* who may ask is decided by SO_PEERCRED */
    }

    /* Charger in, charger out: the kernel announces a power_supply
     * change on the uevent socket. Listening costs nothing; polling every
     * few seconds would cost wakeups on the one machine where they
     * matter. A minute-long poll stays as the backstop for a dropped
     * event. */
    long ue = lp_socket(AF_NETLINK_, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT_);
    if (ue >= 0) {
        struct { u16 fam, pad; u32 pid, groups; } nl = { AF_NETLINK_, 0, 0, 1 };
        if (lp_bind((int)ue, &nl, sizeof nl) < 0) {
            lp_close((int)ue);
            ue = -1;
        }
    }

    for (;;) {
        lp_pollfd_t fds[2];
        unsigned nf = 0;
        if (ls >= 0) { fds[nf].fd = (int)ls; fds[nf].events = LP_POLLIN; fds[nf].revents = 0; nf++; }
        if (ue >= 0) { fds[nf].fd = (int)ue; fds[nf].events = LP_POLLIN; fds[nf].revents = 0; nf++; }
        long r = lp_poll(fds, nf, 60000);

        bool recheck = (r == 0);
        for (unsigned i = 0; r > 0 && i < nf; i++) {
            if (!(fds[i].revents & LP_POLLIN))
                continue;
            if (fds[i].fd == (int)ue) {
                char ev[2048];
                long n = lp_recvfrom((int)ue, ev, sizeof ev - 1, 0, NULL, NULL);
                if (n > 0) {
                    ev[n] = '\0';
                    for (long j = 0; j < n; j += (long)strlen(ev + j) + 1)
                        if (strcmp(ev + j, "SUBSYSTEM=power_supply") == 0)
                            recheck = true;
                }
            } else {
                long c = lp_accept((int)ls, NULL, NULL, 0);
                if (c < 0)
                    continue;
                /* One short line, and a client that does not send it
                 * within a second is dropped rather than waited for. */
                s64 tv[2] = { 1, 0 };
                lp_setsockopt((int)c, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);
                char req[64], reply[160];
                long n = lp_read((int)c, req, sizeof req - 1);
                u32 uid = 65534;
                if (n > 0 && peer_uid((int)c, &uid)) {
                    req[n] = '\0';
                    if (strncmp(req, "status", 6) == 0)
                        json_status((int)c, prof);
                    else {
                        handle(req, uid, &prof, reply, sizeof reply);
                        lp_write((int)c, reply, strlen(reply));
                    }
                }
                lp_close((int)c);
            }
        }

        if (recheck) {
            read_power(&pw);
            if (pw.ac != last_ac) {
                last_ac = pw.ac;
                if (prof == P_AUTO) {
                    apply(effective(prof, &pw));
                    printf("lp-tune: %s - now %s\n",
                           pw.ac ? "on the charger" : "on battery",
                           PNAME[effective(prof, &pw)]);
                }
            }
        }
    }
}

/* Ask the running daemon. Returns false when there is none to ask. */
static bool ask_daemon(const char *line, char *reply, size_t n)
{
    char sp[256];
    rooted(SOCK_PATH, sp, sizeof sp);
    sun_t sa;
    if (!sun_fill(&sa, sp))
        return false;
    long fd = lp_socket(AF_UNIX_, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    if (lp_connect((int)fd, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return false;
    }
    lp_write((int)fd, line, strlen(line));
    long got = 0, r;
    while (got < (long)n - 1 && (r = lp_read((int)fd, reply + got, n - 1 - (size_t)got)) > 0)
        got += r;
    reply[got > 0 ? got : 0] = '\0';
    lp_close((int)fd);
    return got > 0;
}

static void usage(void)
{
    printf("usage: lp-tune [status [--json]]\n"
           "       lp-tune set auto|balanced|saver|performance\n"
           "       lp-tune brightness get | set N|+N|-N\n"
           "       lp-tune -d\n"
           "\n"
           "  auto          balanced on the charger, saver on battery (default)\n"
           "  balanced      power-managed, turbo on\n"
           "  saver         longest battery: no turbo, deeper link and CPU states\n"
           "  performance   turbo, no ASPM, no audio power save\n");
}

int main(int argc, char **argv)
{
    int a = 1;
    while (a < argc && argv[a][0] == '-' && argv[a][1] == '-') {
        if (strcmp(argv[a], "--sysfs") == 0 && a + 1 < argc) {
            strlcpy(root, argv[a + 1], sizeof root);
            a += 2;
        } else if (strcmp(argv[a], "--dry-run") == 0) {
            dry_run = true;
            a++;
        } else if (strcmp(argv[a], "--help") == 0) {
            usage();
            return 0;
        } else {
            break;
        }
    }
    const char *cmd = a < argc ? argv[a] : "status";

    if (strcmp(cmd, "-d") == 0)
        return daemon_main();

    if (strcmp(cmd, "status") == 0) {
        bool json = a + 1 < argc && strcmp(argv[a + 1], "--json") == 0;
        char reply[512];
        /* Prefer the daemon's view: it knows the profile it is holding,
         * which may be newer than the file if a write failed. */
        if (json) {
            if (ask_daemon("status\n", reply, sizeof reply))
                printf("%s", reply);
            else
                json_status(STDOUT_FILENO, load_profile());
        } else {
            human_status(load_profile());
        }
        return 0;
    }

    if (strcmp(cmd, "set") == 0 || strcmp(cmd, "brightness") == 0) {
        char line[64], reply[160];
        if (strcmp(cmd, "brightness") == 0) {
            const char *sub = a + 1 < argc ? argv[a + 1] : "get";
            if (strcmp(sub, "get") == 0) {
                int b = brightness_get();
                if (b < 0) {
                    dprintf(STDERR_FILENO, "lp-tune: no backlight on this machine\n");
                    return 1;
                }
                printf("%d\n", b);
                return 0;
            }
            if (strcmp(sub, "set") != 0 || a + 2 >= argc) {
                usage();
                return 2;
            }
            snprintf(line, sizeof line, "brightness %s\n", argv[a + 2]);
        } else {
            if (a + 1 >= argc) { usage(); return 2; }
            snprintf(line, sizeof line, "set %s\n", argv[a + 1]);
        }

        if (!dry_run && ask_daemon(line, reply, sizeof reply)) {
            if (strncmp(reply, "ok", 2) == 0) {
                printf("%s", reply + 3);
                return 0;
            }
            dprintf(STDERR_FILENO, "lp-tune: %s", reply + 4);
            return 1;
        }
        /* No daemon: root may act directly (the boot, a rescue shell,
         * a test with --sysfs); anyone else has to have the daemon.
         * --sysfs is not an exception - it only moves where things are
         * written, and a rule that bends for a test flag is not a rule. */
        if (lp_getuid() != 0) {
            dprintf(STDERR_FILENO, "lp-tune: the lp-tune service is not running,"
                    " and only root can do this without it\n");
            return 1;
        }
        profile_t prof = load_profile();
        handle(line, 0, &prof, reply, sizeof reply);
        if (strncmp(reply, "ok", 2) == 0) {
            if (!dry_run) printf("%s", reply + 3);
            printf("  (%d written, %d not on this machine, %d refused)\n",
                   n_written, n_absent, n_refused);
            return n_refused ? 1 : 0;
        }
        dprintf(STDERR_FILENO, "lp-tune: %s", reply + 4);
        return 1;
    }

    usage();
    return 2;
}
