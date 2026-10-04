/* battery - how much is left, and how long that is.
 *
 *   battery              every battery and charger the kernel knows
 *   battery -p           the charge alone, "73", for scripts and prompts
 *   battery --sysfs DIR  read DIR/sys/class/power_supply instead (tests)
 *
 * Everything is in /sys/class/power_supply, one directory per battery or
 * charger, one value per file; this only reads it and does the sums a
 * person would: watts from microwatts, hours left from energy and power,
 * and how worn the battery is from what it holds now against what it
 * held new.
 *
 * Which files there are depends on the battery. Most laptops report
 * energy (µWh) and power (µW); some report charge (µAh) and current (µA)
 * instead, and then it is the same arithmetic with the voltage in it.
 * Whatever is missing is left out of the output rather than shown as 0.
 *
 * A Pi, a desktop and a virtual machine (UTM, QEMU) have no battery, and
 * it says so - exit status 1, so `battery -p || echo mains` works.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static char root[256] = "";

static bool rd(const char *dir, const char *name, char *buf, size_t n)
{
    char p[768];
    snprintf(p, sizeof p, "%s/sys/class/power_supply/%s/%s", root, dir, name);
    long fd = lp_open(p, O_RDONLY, 0);
    if (fd < 0)
        return false;
    long got = lp_read((int)fd, buf, n - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return buf[0] != '\0';
}

/* A number from a file, or -1 when the file is not there. */
static s64 num(const char *dir, const char *name)
{
    char b[48];
    if (!rd(dir, name, b, sizeof b))
        return -1;
    s64 v = 0;
    bool neg = false;
    const char *s = b;
    if (*s == '-') { neg = true; s++; }
    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* energy (µWh) at a power (µW) as hours and minutes, rounded - in
 * whole numbers, since the Pi Zero's 32-bit build has no float runtime */
static void hm(s64 energy, s64 power, char *out, size_t n)
{
    s64 mins = (energy * 120 / power + 1) / 2;
    snprintf(out, n, "%lldh %02lldm", (long long)(mins / 60), (long long)(mins % 60));
}

/* One battery. Returns its charge in percent, or -1. */
static int show_battery(const char *d)
{
    char v[96];
    s64 cap = num(d, "capacity");
    char status[32] = "";
    rd(d, "status", status, sizeof status);

    /* energy in µWh and power in µW, or charge in µAh and current in µA
     * with the voltage (µV) to turn them into the same thing. */
    s64 volt = num(d, "voltage_now");
    s64 e_now = num(d, "energy_now"), e_full = num(d, "energy_full");
    s64 e_design = num(d, "energy_full_design"), pw = num(d, "power_now");
    if (e_now < 0 && num(d, "charge_now") >= 0 && volt > 0) {
        e_now    = num(d, "charge_now") * volt / 1000000;
        e_full   = num(d, "charge_full") >= 0 ? num(d, "charge_full") * volt / 1000000 : -1;
        e_design = num(d, "charge_full_design") >= 0 ? num(d, "charge_full_design") * volt / 1000000 : -1;
        s64 cur  = num(d, "current_now");
        pw       = cur >= 0 ? (cur < 0 ? -cur : cur) * volt / 1000000 : -1;
    }
    if (pw < 0) pw = -pw;
    if (cap < 0 && e_now >= 0 && e_full > 0)
        cap = e_now * 100 / e_full;

    printf("%s\n", d);
    if (cap >= 0) {
        char bar[21];
        int filled = (int)(cap / 5);
        for (int i = 0; i < 20; i++) bar[i] = i < filled ? '#' : '.';
        bar[20] = '\0';
        printf("  charge       %lld%%   [%s]\n", (long long)cap, bar);
    }
    if (status[0])
        printf("  status       %s\n", status);
    if (pw > 0) {
        printf("  power        %lld.%02lld W\n", (long long)(pw / 1000000),
               (long long)(pw % 1000000 / 10000));
        char t[32];
        if (strcmp(status, "Discharging") == 0 && e_now > 0) {
            hm(e_now, pw, t, sizeof t);
            printf("  time left    %s   at this rate\n", t);
        } else if (strcmp(status, "Charging") == 0 && e_full > e_now && e_now >= 0) {
            hm(e_full - e_now, pw, t, sizeof t);
            printf("  until full   %s   at this rate\n", t);
        }
    }
    if (e_now >= 0 && e_full > 0)
        printf("  energy       %lld.%01lld of %lld.%01lld Wh\n",
               (long long)(e_now / 1000000), (long long)(e_now % 1000000 / 100000),
               (long long)(e_full / 1000000), (long long)(e_full % 1000000 / 100000));
    if (e_full > 0 && e_design > 0)
        printf("  health       %lld%%   of what it held new (%lld.%01lld Wh)\n",
               (long long)(e_full * 100 / e_design),
               (long long)(e_design / 1000000), (long long)(e_design % 1000000 / 100000));
    s64 cyc = num(d, "cycle_count");
    if (cyc > 0)
        printf("  cycles       %lld\n", (long long)cyc);
    if (volt > 0)
        printf("  voltage      %lld.%02lld V\n", (long long)(volt / 1000000),
               (long long)(volt % 1000000 / 10000));
    if (rd(d, "technology", v, sizeof v))
        printf("  chemistry    %s\n", v);
    char mk[48] = "", md[48] = "";
    rd(d, "manufacturer", mk, sizeof mk);
    rd(d, "model_name", md, sizeof md);
    if (mk[0] || md[0])
        printf("  made by      %s%s%s\n", mk, mk[0] && md[0] ? " " : "", md);
    return (int)cap;
}

int main(int argc, char **argv)
{
    bool pct_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--percent") == 0)
            pct_only = true;
        else if (strcmp(argv[i], "--sysfs") == 0 && i + 1 < argc)
            strlcpy(root, argv[++i], sizeof root);
        else {
            printf("Usage: battery [-p] [--sysfs DIR]\n"
                   "The charge, time left, health and charger of every battery.\n\n"
                   "  -p    the charge alone, as a number (exit 1 without a battery)\n");
            return strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ? 0 : 2;
        }
    }

    char path[512];
    snprintf(path, sizeof path, "%s/sys/class/power_supply", root);
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    int batteries = 0, total_pct = 0;
    char names[16][64];
    int nn = 0;
    if (fd >= 0) {
        char buf[4096];
        for (;;) {
            long n = sys_getdents((int)fd, buf, sizeof buf);
            if (n <= 0)
                break;
            for (long off = 0; off < n; ) {
                char *rec = buf + off;
                off += *(u16 *)(rec + DIRENT_RECLEN);
                const char *name = rec + DIRENT_NAME;
                if (name[0] == '.' || nn >= 16)
                    continue;
                strlcpy(names[nn++], name, sizeof names[0]);
            }
        }
        lp_close((int)fd);
    }

    /* Chargers first in a sentence, batteries after in full. */
    bool any_charger = false, on_mains = false;
    for (int i = 0; i < nn; i++) {
        char type[32];
        if (!rd(names[i], "type", type, sizeof type) || strcmp(type, "Battery") == 0)
            continue;
        any_charger = true;
        if (num(names[i], "online") == 1)
            on_mains = true;
    }
    for (int i = 0; i < nn; i++) {
        char type[32], scope[32] = "";
        if (!rd(names[i], "type", type, sizeof type) || strcmp(type, "Battery") != 0)
            continue;
        /* A mouse's or a keyboard's battery is a battery too, but not
         * this machine's ("Device" scope). */
        if (rd(names[i], "scope", scope, sizeof scope) && strcmp(scope, "Device") == 0)
            continue;
        if (pct_only) {
            s64 c = num(names[i], "capacity");
            if (c >= 0) { total_pct += (int)c; batteries++; }
            continue;
        }
        if (batteries) printf("\n");
        int c = show_battery(names[i]);
        if (c >= 0) total_pct += c;
        batteries++;
    }

    if (pct_only) {
        if (!batteries) return 1;
        printf("%d\n", total_pct / batteries);
        return 0;
    }
    if (!batteries) {
        printf("no battery - this machine runs from mains power%s\n",
               any_charger ? "" : "\n  (a Pi, a desktop or a virtual machine: UTM and QEMU emulate none)");
        return 1;
    }
    if (any_charger)
        printf("\ncharger        %s\n", on_mains ? "plugged in" : "not plugged in");
    return 0;
}
