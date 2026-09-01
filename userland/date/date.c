/* date - show and set the clock.
 *
 *   date                            current time in the configured zone
 *   date -u                         show UTC
 *   date -e                         unix seconds only
 *   date -s "2026-09-01 12:34:56"   set the clock (in the configured zone)
 *   date -z                         show the current zone
 *   date -z list                    list the zones you can pick
 *   date -z Asia/Seoul              pick a zone by name
 *   date -z +9                      or by raw offset
 *
 * The zone is stored in /data/timezone so it survives a reboot.
 *
 * On the clock itself see ntp(1). This board has no battery-backed
 * clock, so time stops when the power goes.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define TZ_FILE     "/data/timezone"
#define CLOCK_FILE  "/data/.clock"

/* Anything before 2020 means the clock was never set. */
#define SANE_MIN    1577836800LL

/* Time zone table.
 *
 * Doing this properly needs tzdata, which is tens of megabytes. There is
 * no room for that here, so we use fixed offsets only. The cost is that
 * daylight saving is not tracked - zones that use it are marked, and in
 * summer you shift by hand. Korea, Japan, China and India have no DST,
 * so those are always exact. */
typedef struct {
    const char *name;      /* what you type to pick it */
    const char *abbr;      /* short name, shown with the time */
    int         minutes;   /* offset from UTC */
    bool        dst;       /* uses daylight saving (we do not follow it) */
} zone_t;

static const zone_t ZONES[] = {
    { "UTC",                 "UTC",   0,           false },
    { "Asia/Seoul",          "KST",   9 * 60,      false },
    { "Asia/Tokyo",          "JST",   9 * 60,      false },
    { "Asia/Shanghai",       "CST",   8 * 60,      false },
    { "Asia/Hong_Kong",      "HKT",   8 * 60,      false },
    { "Asia/Taipei",         "TWT",   8 * 60,      false },
    { "Asia/Singapore",      "SGT",   8 * 60,      false },
    { "Asia/Bangkok",        "ICT",   7 * 60,      false },
    { "Asia/Jakarta",        "WIB",   7 * 60,      false },
    { "Asia/Kolkata",        "IST",   5 * 60 + 30, false },
    { "Asia/Kathmandu",      "NPT",   5 * 60 + 45, false },
    { "Asia/Dubai",          "GST",   4 * 60,      false },
    { "Europe/Moscow",       "MSK",   3 * 60,      false },
    { "Europe/Istanbul",     "TRT",   3 * 60,      false },
    { "Europe/Berlin",       "CET",   1 * 60,      true  },
    { "Europe/Paris",        "CET",   1 * 60,      true  },
    { "Europe/London",       "GMT",   0,           true  },
    { "America/Sao_Paulo",   "BRT",  -3 * 60,      false },
    { "America/New_York",    "EST",  -5 * 60,      true  },
    { "America/Chicago",     "CST",  -6 * 60,      true  },
    { "America/Denver",      "MST",  -7 * 60,      true  },
    { "America/Los_Angeles", "PST",  -8 * 60,      true  },
    { "Pacific/Honolulu",    "HST", -10 * 60,      false },
    { "Australia/Sydney",    "AEST", 10 * 60,      true  },
    { "Pacific/Auckland",    "NZST", 12 * 60,      true  },
};
#define NZONES ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

static const char *WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* Current zone, filled in by load_zone(). */
static int  tz_minutes = 0;
static char tz_label[32] = "UTC";

/* Case-insensitive compare, so "asia/seoul" works too. */
static bool eq_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)    return false;
        if (x == '\0') return true;
    }
}

static const zone_t *find_zone(const char *name)
{
    for (int i = 0; i < NZONES; i++)
        if (eq_ci(ZONES[i].name, name) || eq_ci(ZONES[i].abbr, name))
            return &ZONES[i];
    return NULL;
}

static void offset_text(int minutes, char *buf, size_t cap)
{
    int a = minutes < 0 ? -minutes : minutes;
    snprintf(buf, cap, "UTC%c%d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
}

static void list_zones(void)
{
    printf("Zones you can pick (fixed offset, DST not tracked):\n\n");
    for (int i = 0; i < NZONES; i++) {
        char off[16];
        offset_text(ZONES[i].minutes, off, sizeof(off));
        printf("  %-20s %-5s %-10s%s\n",
               ZONES[i].name, ZONES[i].abbr, off,
               ZONES[i].dst ? "uses DST - shift by hand in summer" : "");
    }
    printf("\n  date -z Asia/Seoul     pick by name\n");
    printf("  date -z KST            or by short name\n");
    printf("  date -z +9             or by raw offset\n");
}

/* The file holds "<minutes> <label>". The label is only for display, so
 * a file with just a number still works. */
static void load_zone(void)
{
    char buf[64];
    long fd = lp_open(TZ_FILE, O_RDONLY, 0);
    if (fd < 0)
        return;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    tz_minutes = atoi(buf);

    char *p = buf;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t')       p++;
    char *end = p;
    while (*end && *end != '\n' && *end != ' ') end++;
    *end = '\0';

    if (*p)
        strlcpy(tz_label, p, sizeof(tz_label));
    else
        offset_text(tz_minutes, tz_label, sizeof(tz_label));
}

static bool save_zone(int minutes, const char *label)
{
    char buf[80];
    int  len = snprintf(buf, sizeof(buf), "%d %s\n", minutes, label);

    long fd = lp_open(TZ_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "date: cannot write %s (%ld)\n"
                "      is /data mounted?\n", TZ_FILE, -fd);
        return false;
    }
    lp_write((int)fd, buf, (size_t)len);
    lp_close((int)fd);
    lp_sync();
    return true;
}

/* Accepts "+9", "-3", "+05:30", "9". */
static bool parse_offset(const char *s, int *out)
{
    int sign = 1;
    if      (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    if (*s < '0' || *s > '9')
        return false;

    int hh = 0;
    while (*s >= '0' && *s <= '9')
        hh = hh * 10 + (*s++ - '0');

    int mm = 0;
    if (*s == ':') {
        s++;
        if (*s < '0' || *s > '9')
            return false;
        while (*s >= '0' && *s <= '9')
            mm = mm * 10 + (*s++ - '0');
    }
    if (*s != '\0' || hh > 14 || mm > 59)
        return false;

    *out = sign * (hh * 60 + mm);
    return true;
}

/* Accepts "2026-09-01 12:34:56", "2026-09-01T12:34:56" or "2026-09-01". */
static bool parse_datetime(const char *s, lp_tm_t *tm)
{
    int vals[6] = { 0, 0, 0, 0, 0, 0 };
    int n = 0;
    const char *p = s;

    while (n < 6) {
        if (*p < '0' || *p > '9')
            break;
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        vals[n++] = v;
        if (*p == '-' || *p == ' ' || *p == ':' || *p == 'T')
            p++;
        else
            break;
    }
    if (*p != '\0' || n < 3)
        return false;

    tm->year = vals[0]; tm->mon = vals[1]; tm->day  = vals[2];
    tm->hour = vals[3]; tm->min = vals[4]; tm->sec  = vals[5];
    tm->wday = 0;

    if (tm->year < 1970 || tm->mon < 1 || tm->mon > 12 ||
        tm->day  < 1    || tm->day > 31 || tm->hour > 23 ||
        tm->min  > 59   || tm->sec > 60)
        return false;
    return true;
}

static void print_time(s64 t, int minutes, const char *label)
{
    lp_tm_t tm;
    lp_gmtime(t + (s64)minutes * 60, &tm);

    printf("%s %d-%02d-%02d %02d:%02d:%02d %s\n",
           WDAY[tm.wday], tm.year, tm.mon, tm.day,
           tm.hour, tm.min, tm.sec, label);

    if (t < SANE_MIN)
        printf("clock is not set - run 'ntp', or 'date -s \"2026-09-01 12:00:00\"'\n");
}

/* Remember the time so the next boot can pick up where this one left off.
 * ntp reads the same file. */
static void save_clock(s64 t)
{
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);
    long fd = lp_open(CLOCK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("(/data is not mounted - this will not survive a reboot)\n");
        return;
    }
    lp_write((int)fd, buf, (size_t)len);
    lp_close((int)fd);
    lp_sync();
}

static void usage(void)
{
    printf("usage:\n");
    printf("  date                            show the time\n");
    printf("  date -u                         show UTC\n");
    printf("  date -e                         unix seconds\n");
    printf("  date -s \"2026-09-01 12:34:56\"   set the clock\n");
    printf("  date -z                         show the time zone\n");
    printf("  date -z list                    list zones\n");
    printf("  date -z Asia/Seoul              set the zone\n\n");
    printf("to set the clock from the network: ntp\n");
}

int main(int argc, char **argv)
{
    load_zone();

    if (argc == 1) {
        print_time(lp_time(), tz_minutes, tz_label);
        return 0;
    }

    const char *opt = argv[1];

    if (strcmp(opt, "-h") == 0 || strcmp(opt, "--help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(opt, "-u") == 0) {
        print_time(lp_time(), 0, "UTC");
        return 0;
    }

    if (strcmp(opt, "-e") == 0) {
        printf("%lld\n", (long long)lp_time());
        return 0;
    }

    if (strcmp(opt, "-z") == 0) {
        if (argc < 3) {
            char off[16];
            offset_text(tz_minutes, off, sizeof(off));
            printf("%s (%s)\n", tz_label, off);
            printf("run 'date -z list' to see the choices\n");
            return 0;
        }

        if (strcmp(argv[2], "list") == 0) {
            list_zones();
            return 0;
        }

        const zone_t *z = find_zone(argv[2]);
        int   minutes;
        char  label[32];

        if (z) {
            minutes = z->minutes;
            strlcpy(label, z->abbr, sizeof(label));
        } else if (parse_offset(argv[2], &minutes)) {
            offset_text(minutes, label, sizeof(label));
        } else {
            dprintf(STDERR_FILENO,
                    "date: unknown time zone: %s\n"
                    "      try 'date -z list', or an offset like +9\n", argv[2]);
            return 2;
        }

        if (!save_zone(minutes, label))
            return 1;

        tz_minutes = minutes;
        strlcpy(tz_label, label, sizeof(tz_label));

        if (z && z->dst)
            printf("note: %s uses daylight saving. We keep a fixed offset,\n"
                   "      so shift by one hour yourself in summer.\n", z->name);

        print_time(lp_time(), tz_minutes, tz_label);
        return 0;
    }

    if (strcmp(opt, "-s") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO,
                    "date: give a time to set.\n"
                    "      e.g. date -s \"2026-09-01 12:34:56\"\n");
            return 2;
        }

        lp_tm_t tm;
        if (!parse_datetime(argv[2], &tm)) {
            dprintf(STDERR_FILENO,
                    "date: cannot read that time: %s\n"
                    "      e.g. \"2026-09-01 12:34:56\" or \"2026-09-01\"\n",
                    argv[2]);
            return 2;
        }

        /* The input is in the configured zone; the kernel wants UTC. */
        s64 t = lp_timegm(&tm) - (s64)tz_minutes * 60;

        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO, "date: cannot set the clock (are you root?)\n");
            return 1;
        }
        print_time(t, tz_minutes, tz_label);
        save_clock(t);
        return 0;
    }

    dprintf(STDERR_FILENO, "date: unknown option: %s\n", opt);
    usage();
    return 2;
}
