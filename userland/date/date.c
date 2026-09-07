/* date - show and set the clock.
 *
 *   date                            Mon Sep  7 09:51:39 UTC 2026
 *   date +%Y-%m-%d                  any format string strftime knows
 *   date -d "2 days ago" +%F        a time other than now
 *   date -u                         show UTC
 *   date -R                         the shape an email header wants
 *   date -e                         unix seconds only (ours, not GNU's)
 *   date -s "2026-09-01 12:34:56"   set the clock (in the configured zone)
 *   date -z                         show the current zone
 *   date -z list                    list the zones you can pick
 *   date -z Asia/Seoul              pick a zone by name
 *   date -z +9                      or by raw offset
 *
 * The zone is stored in /data/timezone - or /etc/timezone on a machine
 * whose root is a real disk - so it survives a reboot. It is written by
 * rename, not in place: a power cut in the middle of an in-place write
 * leaves an empty file, and an empty timezone reads back as UTC.
 *
 * On the clock itself see ntp(1). This board has no battery-backed
 * clock, so time stops when the power goes.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* The zone and the last-known time both have to outlive a reboot, and
 * where that is depends on the machine: on a RAM root only /data does,
 * on a disk root there is no /data and /etc is ordinary. lp_setting_path
 * picks whichever one this machine actually keeps. */
#define TZ_NAME     "timezone"
#define CLOCK_NAME  ".clock"

/* Anything before 2020 means the clock was never set. */
#define SANE_MIN    1577836800LL

/* Time zone table.
 *
 * Doing this properly needs tzdata, which is tens of megabytes and this
 * root lives in RAM. So the offsets are here and the daylight-saving
 * rules are in the libc, computed rather than looked up.
 *
 * This used to say "zones that use DST are marked, shift by hand in
 * summer". That is not a time zone, it is a chore with a deadline:
 * twice a year every timestamp on the machine is an hour wrong until
 * somebody remembers, and a log written across the change cannot be
 * read at all. The four rules below cover every zone in this table and
 * none of them has changed in twenty years. */
typedef struct {
    const char *name;      /* what you type to pick it */
    const char *abbr;      /* short name in winter */
    const char *summer;    /* short name in summer, or the same */
    int         minutes;   /* standard offset from UTC */
    lp_dst_t    rule;      /* which daylight-saving rule, if any */
} zone_t;

static const zone_t ZONES[] = {
    { "UTC",                 "UTC",  "UTC",    0,           LP_DST_NONE },
    { "Asia/Seoul",          "KST",  "KST",    9 * 60,      LP_DST_NONE },
    { "Asia/Tokyo",          "JST",  "JST",    9 * 60,      LP_DST_NONE },
    { "Asia/Shanghai",       "CST",  "CST",    8 * 60,      LP_DST_NONE },
    { "Asia/Hong_Kong",      "HKT",  "HKT",    8 * 60,      LP_DST_NONE },
    { "Asia/Taipei",         "TWT",  "TWT",    8 * 60,      LP_DST_NONE },
    { "Asia/Singapore",      "SGT",  "SGT",    8 * 60,      LP_DST_NONE },
    { "Asia/Bangkok",        "ICT",  "ICT",    7 * 60,      LP_DST_NONE },
    { "Asia/Jakarta",        "WIB",  "WIB",    7 * 60,      LP_DST_NONE },
    { "Asia/Kolkata",        "IST",  "IST",    5 * 60 + 30, LP_DST_NONE },
    { "Asia/Kathmandu",      "NPT",  "NPT",    5 * 60 + 45, LP_DST_NONE },
    { "Asia/Dubai",          "GST",  "GST",    4 * 60,      LP_DST_NONE },
    { "Europe/Moscow",       "MSK",  "MSK",    3 * 60,      LP_DST_NONE },
    { "Europe/Istanbul",     "TRT",  "TRT",    3 * 60,      LP_DST_NONE },
    { "Europe/Berlin",       "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Paris",        "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Madrid",       "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Rome",         "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Amsterdam",    "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Warsaw",       "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Stockholm",    "CET",  "CEST",   1 * 60,      LP_DST_EU   },
    { "Europe/Athens",       "EET",  "EEST",   2 * 60,      LP_DST_EU   },
    { "Europe/Helsinki",     "EET",  "EEST",   2 * 60,      LP_DST_EU   },
    { "Europe/Lisbon",       "WET",  "WEST",   0,           LP_DST_EU   },
    { "Europe/Dublin",       "GMT",  "IST",    0,           LP_DST_EU   },
    { "Europe/London",       "GMT",  "BST",    0,           LP_DST_EU   },
    { "America/Sao_Paulo",   "BRT",  "BRT",   -3 * 60,      LP_DST_NONE },
    { "America/Bogota",      "COT",  "COT",   -5 * 60,      LP_DST_NONE },
    { "America/Toronto",     "EST",  "EDT",   -5 * 60,      LP_DST_US   },
    { "America/New_York",    "EST",  "EDT",   -5 * 60,      LP_DST_US   },
    { "America/Chicago",     "CST",  "CDT",   -6 * 60,      LP_DST_US   },
    { "America/Mexico_City", "CST",  "CST",   -6 * 60,      LP_DST_NONE },
    { "America/Denver",      "MST",  "MDT",   -7 * 60,      LP_DST_US   },
    { "America/Phoenix",     "MST",  "MST",   -7 * 60,      LP_DST_NONE },
    { "America/Vancouver",   "PST",  "PDT",   -8 * 60,      LP_DST_US   },
    { "America/Los_Angeles", "PST",  "PDT",   -8 * 60,      LP_DST_US   },
    { "America/Anchorage",   "AKST", "AKDT",  -9 * 60,      LP_DST_US   },
    { "Pacific/Honolulu",    "HST",  "HST",  -10 * 60,      LP_DST_NONE },
    { "Australia/Perth",     "AWST", "AWST",   8 * 60,      LP_DST_NONE },
    { "Australia/Brisbane",  "AEST", "AEST",  10 * 60,      LP_DST_NONE },
    { "Australia/Sydney",    "AEST", "AEDT",  10 * 60,      LP_DST_AU   },
    { "Australia/Melbourne", "AEST", "AEDT",  10 * 60,      LP_DST_AU   },
    { "Pacific/Auckland",    "NZST", "NZDT",  12 * 60,      LP_DST_NZ   },
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
    printf("Zones you can pick. Daylight saving is followed"
           " automatically.\n\n");
    for (int i = 0; i < NZONES; i++) {
        char off[16];
        offset_text(ZONES[i].minutes, off, sizeof(off));
        printf("  %-20s %-5s %-10s%s\n",
               ZONES[i].name, ZONES[i].abbr, off,
               ZONES[i].rule != LP_DST_NONE ? ZONES[i].summer : "");
    }
    printf("\n  date -z Asia/Seoul     pick by name\n");
    printf("  date -z KST            or by short name\n");
    printf("  date -z +9             or by raw offset\n");
}

/* The file holds "<minutes> <label>". The label is only for display, so
 * a file with just a number still works. */
/* The libc reads the same file and applies the daylight-saving rule, so
 * this is the whole of it now. Parsing it a second time here is how the
 * two used to drift. */
static void load_zone(void)
{
    lp_tz_forget();
    s64 now = lp_time();
    tz_minutes = lp_tz_offset(now);
    strlcpy(tz_label, lp_tz_label(now), sizeof(tz_label));
}

/* "<minutes> <winter label> <rule> <summer label>".
 *
 * The rule and the summer name are on the same line because every
 * program on the machine reads this file through lp_localtime, and a
 * second file to keep in step is a second file to get out of step. */
static bool save_zone(int minutes, const char *label,
                      const char *rule, const char *summer)
{
    char buf[96];
    int  len = snprintf(buf, sizeof(buf), "%d %s %s %s\n",
                        minutes, label, rule ? rule : "-",
                        summer ? summer : label);

    char path[256];
    lp_setting_path(TZ_NAME, path, sizeof path);

    if (!lp_write_file_atomic(path, buf, (size_t)len)) {
        dprintf(STDERR_FILENO,
                "date: cannot write %s\n"
                "      nothing writable survives a reboot on this machine -\n"
                "      is /data mounted?\n", path);
        return false;
    }
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

/* Remember the time so the next boot can pick up where this one left
 * off. Two places, because the two machines this runs on are different:
 *
 *   the hardware clock  A PC and an EC2 instance have one with a
 *                       battery, and it keeps counting while the power
 *                       is off. That is the only way a machine switched
 *                       on a week later knows a week has passed.
 *   /data/.clock        A Pi Zero 2 W has no such clock. The saved
 *                       timestamp does not advance while the power is
 *                       off, but it beats starting at 1970 - which
 *                       fails every HTTPS handshake outright.
 *
 * Whichever exists gets written. ntp reads the same file. */
static void save_clock(s64 t)
{
    bool rtc = lp_rtc_write(t);

    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);

    char path[256];
    lp_setting_path(CLOCK_NAME, path, sizeof path);

    if (!lp_write_file_atomic(path, buf, (size_t)len) && !rtc)
        printf("(no hardware clock and nothing writable that survives a "
               "reboot - this time will be gone at the next boot)\n");
}

static const char *WDAY_FULL[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday" };
static const char *MON_ABBR[13] = { "", "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *MON_FULL[13] = { "", "January","February","March","April",
                                    "May","June","July","August","September",
                                    "October","November","December" };

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int yday_of(const lp_tm_t *tm)
{
    static const int cum[13] = { 0,0,31,59,90,120,151,181,212,243,273,304,334 };
    int d = cum[tm->mon] + tm->day;
    if (tm->mon > 2 && leap(tm->year)) d++;
    return d;                                   /* 1..366 */
}

/* ── strftime ─────────────────────────────────────────────────────────
 *
 * `date +%F` is the most-typed form of this command by a wide margin,
 * and the old version answered "unknown option: +%Y-%m-%d". A date that
 * cannot be formatted is a date every script has to work around, so
 * this is the whole set - including the flags (-_0^) and the field
 * width, which is what makes `%-d` print 7 rather than 07.
 *
 * There is no locale here, so %c %x %X and the day and month names are
 * the C locale's, which is what a machine with no locale data should
 * say rather than pretending to know Korean month names.
 */
static void put(char *out, size_t cap, size_t *k, const char *s)
{
    while (*s && *k < cap - 1) out[(*k)++] = *s++;
}

static void put_num(char *out, size_t cap, size_t *k, long long v,
                    int width, char pad, char flag)
{
    if (flag == '-') { width = 0; }
    else if (flag == '_') pad = ' ';
    else if (flag == '0') pad = '0';

    char digits[32];
    int  n = 0;
    bool neg = v < 0;
    unsigned long long a = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    if (a == 0) digits[n++] = '0';
    while (a) { digits[n++] = (char)('0' + a % 10); a /= 10; }

    int len = n + (neg ? 1 : 0);
    if (neg && pad == '0' && *k < cap - 1) out[(*k)++] = '-';
    for (int i = len; i < width && *k < cap - 1; i++) out[(*k)++] = pad;
    if (neg && pad != '0' && *k < cap - 1) out[(*k)++] = '-';
    while (n-- > 0 && *k < cap - 1) out[(*k)++] = digits[n];
}

static void upper_from(char *out, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++)
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 32);
}

/* The ISO week number, and the year it belongs to - which is not always
 * the calendar year: 1 January can be week 52 or 53 of the year before,
 * and 31 December can be week 1 of the year after. Weekdays are counted
 * Monday=1 here because that is what the ISO rule is written in, and
 * getting that off by one turns week 53 into week 1 of the wrong year. */
static void iso_week(const lp_tm_t *tm, int *week, int *year)
{
    int wd = (tm->wday == 0) ? 7 : tm->wday;    /* Mon=1 .. Sun=7 */
    int yd = yday_of(tm);                       /* 1..366 */
    int y  = tm->year;
    int w  = (yd - wd + 10) / 7;

    if (w < 1) {                    /* the last week of the year before */
        y--;
        yd += leap(y) ? 366 : 365;
        w = (yd - wd + 10) / 7;
    } else if (w > 52) {            /* week 53 only if the year has one */
        int days = leap(y) ? 366 : 365;
        if (days - yd < 4 - wd) { w = 1; y++; }
    }
    *week = w;
    *year = y;
}

static size_t fmt_time(char *out, size_t cap, const char *fmt,
                       s64 t, int off, const char *zone)
{
    lp_tm_t tm;
    lp_gmtime(t + (s64)off * 60, &tm);
    size_t k = 0;

    for (const char *p = fmt; *p && k < cap - 1; p++) {
        if (*p != '%') { out[k++] = *p; continue; }
        p++;

        char flag = 0;
        while (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') {
            if (*p == '^' || *p == '#') flag = flag ? flag : '^';
            else flag = *p;
            p++;
        }
        bool upper = false;
        for (const char *q = p - 1; q > fmt && (*q == '^' || *q == '#'); q--) upper = true;
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (!*p) break;

        size_t before = k;
        switch (*p) {
        case 'a': put(out, cap, &k, WDAY[tm.wday % 7]); break;
        case 'A': put(out, cap, &k, WDAY_FULL[tm.wday % 7]); break;
        case 'b': case 'h': put(out, cap, &k, MON_ABBR[tm.mon]); break;
        case 'B': put(out, cap, &k, MON_FULL[tm.mon]); break;
        case 'c': {
            char b[128];
            fmt_time(b, sizeof b, "%a %b %e %H:%M:%S %Y", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'C': put_num(out, cap, &k, tm.year / 100, width ? width : 2, '0', flag); break;
        case 'd': put_num(out, cap, &k, tm.day, width ? width : 2, '0', flag); break;
        case 'D': {
            char b[32];
            fmt_time(b, sizeof b, "%m/%d/%y", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'e': put_num(out, cap, &k, tm.day, width ? width : 2, ' ', flag); break;
        case 'F': {
            char b[32];
            fmt_time(b, sizeof b, "%Y-%m-%d", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'g': { int w, y; iso_week(&tm, &w, &y);
                    put_num(out, cap, &k, y % 100, width ? width : 2, '0', flag); break; }
        case 'G': { int w, y; iso_week(&tm, &w, &y);
                    put_num(out, cap, &k, y, width ? width : 4, '0', flag); break; }
        case 'H': put_num(out, cap, &k, tm.hour, width ? width : 2, '0', flag); break;
        case 'I': { int h = tm.hour % 12; if (!h) h = 12;
                    put_num(out, cap, &k, h, width ? width : 2, '0', flag); break; }
        case 'j': put_num(out, cap, &k, yday_of(&tm), width ? width : 3, '0', flag); break;
        case 'k': put_num(out, cap, &k, tm.hour, width ? width : 2, ' ', flag); break;
        case 'l': { int h = tm.hour % 12; if (!h) h = 12;
                    put_num(out, cap, &k, h, width ? width : 2, ' ', flag); break; }
        case 'm': put_num(out, cap, &k, tm.mon, width ? width : 2, '0', flag); break;
        case 'M': put_num(out, cap, &k, tm.min, width ? width : 2, '0', flag); break;
        case 'n': put(out, cap, &k, "\n"); break;
        case 'N': put_num(out, cap, &k, 0, 9, '0', 0); break;   /* no sub-second clock here */
        case 'p': put(out, cap, &k, tm.hour < 12 ? "AM" : "PM"); break;
        case 'P': put(out, cap, &k, tm.hour < 12 ? "am" : "pm"); break;
        case 'q': put_num(out, cap, &k, (tm.mon + 2) / 3, 0, '0', flag); break;
        case 'r': {
            char b[64];
            fmt_time(b, sizeof b, "%I:%M:%S %p", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'R': {
            char b[32];
            fmt_time(b, sizeof b, "%H:%M", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 's': put_num(out, cap, &k, (long long)t, width, '0', flag); break;
        case 'S': put_num(out, cap, &k, tm.sec, width ? width : 2, '0', flag); break;
        case 't': put(out, cap, &k, "\t"); break;
        case 'T': {
            char b[32];
            fmt_time(b, sizeof b, "%H:%M:%S", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'u': put_num(out, cap, &k, tm.wday == 0 ? 7 : tm.wday, 0, '0', flag); break;
        case 'U': put_num(out, cap, &k, (yday_of(&tm) + 6 - tm.wday) / 7,
                          width ? width : 2, '0', flag); break;
        case 'V': { int w, y; iso_week(&tm, &w, &y);
                    put_num(out, cap, &k, w, width ? width : 2, '0', flag); break; }
        case 'w': put_num(out, cap, &k, tm.wday, 0, '0', flag); break;
        case 'W': put_num(out, cap, &k, (yday_of(&tm) + 6 - ((tm.wday + 6) % 7)) / 7,
                          width ? width : 2, '0', flag); break;
        case 'x': {
            char b[32];
            fmt_time(b, sizeof b, "%m/%d/%y", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'X': {
            char b[32];
            fmt_time(b, sizeof b, "%H:%M:%S", t, off, zone);
            put(out, cap, &k, b);
            break;
        }
        case 'y': put_num(out, cap, &k, tm.year % 100, width ? width : 2, '0', flag); break;
        case 'Y': put_num(out, cap, &k, tm.year, width, '0', flag); break;
        case 'z': {
            char b[16];
            int a = off < 0 ? -off : off;
            snprintf(b, sizeof b, "%c%02d%02d", off < 0 ? '-' : '+', a / 60, a % 60);
            put(out, cap, &k, b);
            break;
        }
        case ':': {
            if (p[1] == 'z') {
                p++;
                char b[16];
                int a = off < 0 ? -off : off;
                snprintf(b, sizeof b, "%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
                put(out, cap, &k, b);
            } else {
                out[k++] = '%';
                if (k < cap - 1) out[k++] = ':';
            }
            break;
        }
        case 'Z': put(out, cap, &k, zone); break;
        case '%': if (k < cap - 1) out[k++] = '%'; break;
        default:
            if (k < cap - 1) out[k++] = '%';
            if (k < cap - 1) out[k++] = *p;
            break;
        }
        if (upper || flag == '^') upper_from(out, before, k);
    }
    out[k] = '\0';
    return k;
}

static void print_time(s64 t, int minutes, const char *label)
{
    char buf[512];
    /* GNU's default format, in the C locale. */
    fmt_time(buf, sizeof buf, "%a %b %e %H:%M:%S %Z %Y", t, minutes, label);
    printf("%s\n", buf);
}

/* Said only about the clock itself. `date -d @0` is a question about
 * 1970, not a machine whose clock stopped there. */
static void warn_if_unset(void)
{
    if (lp_time() < SANE_MIN)
        printf("clock is not set - run 'ntp', or 'date -s \"2026-09-01 12:00:00\"'\n");
}

/* ── -d: a time that is not now ───────────────────────────────────────
 *
 * GNU's -d accepts an entire small language. This accepts the part of
 * it that scripts actually use, and refuses the rest out loud rather
 * than guessing - a date command that silently returns the wrong day is
 * worse than one that says it cannot read the string.
 */
static bool relative_unit(const char *word, s64 *secs)
{
    size_t n = strlen(word);
    char w[32];
    strlcpy(w, word, sizeof w);
    if (n > 1 && w[n - 1] == 's') w[n - 1] = '\0';

    if (strcmp(w, "second") == 0 || strcmp(w, "sec") == 0) { *secs = 1; return true; }
    if (strcmp(w, "minute") == 0 || strcmp(w, "min") == 0) { *secs = 60; return true; }
    if (strcmp(w, "hour") == 0)  { *secs = 3600; return true; }
    if (strcmp(w, "day") == 0)   { *secs = 86400; return true; }
    if (strcmp(w, "week") == 0)  { *secs = 7 * 86400; return true; }
    if (strcmp(w, "fortnight") == 0) { *secs = 14 * 86400; return true; }
    return false;
}

static bool parse_when(const char *s, s64 now, int off, s64 *out)
{
    while (*s == ' ') s++;

    if (*s == '@') {                       /* @1788771305 */
        const char *p = s + 1;
        bool neg = (*p == '-');
        if (neg) p++;
        if (*p < '0' || *p > '9') return false;
        s64 v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        if (*p) return false;
        *out = neg ? -v : v;
        return true;
    }

    if (strcmp(s, "now") == 0)       { *out = now; return true; }
    if (strcmp(s, "today") == 0)     { *out = now; return true; }
    if (strcmp(s, "tomorrow") == 0)  { *out = now + 86400; return true; }
    if (strcmp(s, "yesterday") == 0) { *out = now - 86400; return true; }

    /* An absolute date first: "2026-01-15" starts with digits too, and
     * reading it as "2026 somethings" would quietly give the wrong day. */
    {
        lp_tm_t tm;
        if (parse_datetime(s, &tm)) {
            *out = lp_timelocal(&tm);
            return true;
        }
    }

    /* "2 days ago", "+3 hours", "-1 week", "3 days" */
    {
        const char *p = s;
        int sign = 1;
        if (*p == '+') p++;
        else if (*p == '-') { sign = -1; p++; }
        if (*p >= '0' && *p <= '9') {
            s64 n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            char unit[32];
            size_t k = 0;
            while (*p && *p != ' ' && k < sizeof unit - 1) unit[k++] = *p++;
            unit[k] = '\0';
            s64 secs;
            if (relative_unit(unit, &secs)) {
                while (*p == ' ') p++;
                if (strcmp(p, "ago") == 0) sign = -sign;
                else if (*p) return false;
                *out = now + sign * n * secs;
                return true;
            }
            /* Months and years are not a fixed number of seconds, so
             * they are done on the calendar rather than by arithmetic. */
            if (strcmp(unit, "month") == 0 || strcmp(unit, "months") == 0 ||
                strcmp(unit, "year") == 0  || strcmp(unit, "years") == 0) {
                while (*p == ' ') p++;
                if (strcmp(p, "ago") == 0) sign = -sign;
                else if (*p) return false;
                lp_tm_t tm;
                lp_gmtime(now + (s64)off * 60, &tm);
                if (unit[0] == 'm') {
                    int total = (tm.year * 12 + tm.mon - 1) + (int)(sign * n);
                    tm.year = total / 12;
                    tm.mon  = total % 12 + 1;
                } else {
                    tm.year += (int)(sign * n);
                }
                static const int mdays[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
                int last = mdays[tm.mon];
                if (tm.mon == 2 && leap(tm.year)) last = 29;
                if (tm.day > last) tm.day = last;
                *out = lp_timegm(&tm) - (s64)off * 60;
                return true;
            }
            return false;
        }
    }

    return false;
}

static void usage(void)
{
    printf("Usage: date [OPTION]... [+FORMAT]\n"
           "Display the current time in the given FORMAT, or set the system date.\n\n"
           "  -d, --date=STRING          display time described by STRING, not 'now'\n"
           "  -I[FMT], --iso-8601[=FMT]  ISO 8601 format; FMT is 'hours', 'minutes',\n"
           "                               'date' (the default), 'seconds' or 'ns'\n"
           "  -R, --rfc-email            output in RFC 5322 format\n"
           "      --rfc-3339=FMT         RFC 3339 format; FMT is 'date', 'seconds' or 'ns'\n"
           "  -r, --reference=FILE       display the last modification time of FILE\n"
           "  -s, --set=STRING           set the time described by STRING\n"
           "  -u, --utc, --universal     work in UTC rather than the configured zone\n"
           "      --help     display this help and exit\n\n"
           "FORMAT is a string beginning with +. Interpreted sequences are:\n\n"
           "  %%%%   a literal %%\n"
           "  %%a   abbreviated weekday name (e.g., Sun)\n"
           "  %%A   full weekday name (e.g., Sunday)\n"
           "  %%b   abbreviated month name (e.g., Jan)      %%h  same as %%b\n"
           "  %%B   full month name (e.g., January)\n"
           "  %%c   date and time (e.g., Thu Mar  3 23:05:25 2005)\n"
           "  %%C   century; like %%Y, without the last two digits (e.g., 20)\n"
           "  %%d   day of month (01..31)                   %%e  day of month, space padded\n"
           "  %%D   date; same as %%m/%%d/%%y                   %%F  full date; %%Y-%%m-%%d\n"
           "  %%g   last two digits of the ISO week year    %%G  the ISO week year\n"
           "  %%H   hour (00..23)                           %%I  hour (01..12)\n"
           "  %%j   day of year (001..366)\n"
           "  %%k   hour, space padded ( 0..23)             %%l  hour, space padded ( 1..12)\n"
           "  %%m   month (01..12)                          %%M  minute (00..59)\n"
           "  %%n   a newline                               %%t  a tab\n"
           "  %%N   nanoseconds - always 000000000 here; this clock has no sub-second part\n"
           "  %%p   AM or PM                                %%P  am or pm\n"
           "  %%q   quarter of year (1..4)                  %%r  12-hour clock time\n"
           "  %%R   hour and minute; same as %%H:%%M           %%T  time; same as %%H:%%M:%%S\n"
           "  %%s   seconds since 1970-01-01 00:00 UTC      %%S  second (00..60)\n"
           "  %%u   day of week (1..7); 1 is Monday         %%w  day of week (0..6); 0 is Sunday\n"
           "  %%U   week of year, Sunday first (00..53)     %%W  week of year, Monday first\n"
           "  %%V   ISO week number (01..53)\n"
           "  %%x   date representation (e.g., 12/31/99)    %%X  time representation\n"
           "  %%y   last two digits of year (00..99)        %%Y  year\n"
           "  %%z   +hhmm numeric time zone                 %%:z +hh:mm numeric time zone\n"
           "  %%Z   time zone abbreviation (e.g., KST)\n\n"
           "By default numeric fields are padded with zeroes. These flags may follow '%%':\n"
           "  -  do not pad the field    _  pad with spaces    0  pad with zeros\n"
           "  ^  use upper case if possible\n\n"
           "-d understands: @SECONDS, 'now', 'today', 'tomorrow', 'yesterday',\n"
           "an absolute \"2026-09-01 12:34:56\", and \"N units\" or \"N units ago\" where\n"
           "the unit is second, minute, hour, day, week, month or year. It is not\n"
           "GNU's whole date language - anything else is refused rather than guessed at.\n\n"
           "Two options here are not GNU's, because this machine carries no tzdata:\n"
           "  -e             the time as plain unix seconds\n"
           "  -z [ZONE]      show, list or set the time zone\n"
           "                   date -z list, date -z Asia/Seoul, date -z +9\n"
           "The zone is saved and survives a reboot. Daylight saving is computed from\n"
           "the rule rather than looked up, so the clock shifts by itself.\n\n"
           "To set the clock from the network: ntp\n"
           "To keep it across a power cut on a machine that has a battery: hwclock -w\n");
}

/* -z, kept from before: this system has no tzdata and no /etc/localtime,
 * so there has to be some way to say where the machine is. */
static int do_zone(int argc, char **argv, int at)
{
    if (at >= argc) {
        char off[16];
        offset_text(tz_minutes, off, sizeof(off));
        printf("%s (%s)\n", tz_label, off);
        printf("run 'date -z list' to see the choices\n");
        return 0;
    }
    if (strcmp(argv[at], "list") == 0) { list_zones(); return 0; }

    const zone_t *z = find_zone(argv[at]);
    int   minutes;
    char  label[32];

    if (z) {
        minutes = z->minutes;
        strlcpy(label, z->abbr, sizeof(label));
    } else if (parse_offset(argv[at], &minutes)) {
        offset_text(minutes, label, sizeof(label));
    } else {
        dprintf(STDERR_FILENO,
                "date: unknown time zone: %s\n"
                "      try 'date -z list', or an offset like +9\n", argv[at]);
        return 2;
    }

    const char *rulename = "-";
    if (z) {
        switch (z->rule) {
        case LP_DST_EU: rulename = "EU"; break;
        case LP_DST_US: rulename = "US"; break;
        case LP_DST_AU: rulename = "AU"; break;
        case LP_DST_NZ: rulename = "NZ"; break;
        default:        rulename = "-";  break;
        }
    }
    if (!save_zone(minutes, label, rulename, z ? z->summer : label))
        return 1;

    /* Re-read through the libc so what is printed next is what every
     * other program will now see, rather than what this one just
     * decided. If those two ever differ it is this line that finds out,
     * and finding out immediately is the point. */
    lp_tz_forget();
    s64 now = lp_time();
    tz_minutes = lp_tz_offset(now);
    strlcpy(tz_label, lp_tz_label(now), sizeof(tz_label));

    if (z && z->rule != LP_DST_NONE)
        printf("%s follows daylight saving; the clock shifts by itself.\n", z->name);

    print_time(now, tz_minutes, tz_label);
    return 0;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "date", 1, 'd' }, { "iso-8601", 2, 'I' }, { "rfc-email", 0, 'R' },
        { "rfc-3339", 1, '3' }, { "reference", 1, 'r' }, { "set", 1, 's' },
        { "utc", 0, 'u' }, { "universal", 0, 'u' }, { "debug", 0, 'D' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    load_zone();

    const char *when = NULL, *setstr = NULL, *reffile = NULL;
    const char *iso = NULL, *rfc3339 = NULL;
    bool utc = false, rfc_email = false, zone_cmd = false;
    int  zone_at = 0, real_argc = argc;

    /* -z takes an optional value that may be a whole zone name, and it
     * is ours rather than GNU's, so it is picked off before getopt sees
     * it. Everything after -z belongs to it. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-z") == 0) {
            zone_cmd = true;
            zone_at = i + 1;
            argc = i;                  /* hide the rest from getopt */
            break;
        }
    }

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "d:I::Rr:s:ue", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'd': when = g.arg; break;
        case 'I': iso = g.arg ? g.arg : "date"; break;
        case '3': rfc3339 = g.arg; break;
        case 'R': rfc_email = true; break;
        case 'r': reffile = g.arg; break;
        case 's': setstr = g.arg; break;
        case 'u': utc = true; break;
        case 'D': break;
        case 'e': {                    /* ours: plain unix seconds */
            printf("%lld\n", (long long)lp_time());
            return 0;
        }
        case 'H': usage(); return 0;
        default: lp_getopt_err("date", &g); return 1;
        }
    }

    if (zone_cmd)
        return do_zone(real_argc, argv, zone_at);

    /* The offset of the moment being printed, not of this one. A date in
     * March formatted with September's offset is an hour wrong, and that
     * is exactly the case somebody reaches for `date -d` to check. */
    int off = utc ? 0 : tz_minutes;
    const char *label = utc ? "UTC" : tz_label;

    if (setstr) {
        s64 t;
        if (!parse_when(setstr, lp_time(), off, &t)) {
            dprintf(STDERR_FILENO, "date: invalid date '%s'\n", setstr);
            return 1;
        }
        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO,
                    "date: cannot set date: Operation not permitted\n");
            return 1;
        }
        save_clock(t);
        print_time(t, off, label);
        return 0;
    }

    s64 t = lp_time();
    if (reffile) {
        lp_stat_t st;
        if (lp_stat(reffile, &st, true) < 0) {
            lp_diag("date", NULL, NULL, "cannot stat", reffile, 2);
            return 1;
        }
        t = st.mtime;
    }
    if (when && !parse_when(when, t, off, &t)) {
        dprintf(STDERR_FILENO, "date: invalid date '%s'\n", when);
        return 1;
    }

    if (!utc) {
        off   = lp_tz_offset(t);
        label = lp_tz_label(t);
    }

    /* The operand, if there is one, is the format. */
    const char *fmt = NULL;
    for (int i = g.ind; i < argc; i++) {
        if (argv[i][0] == '+') { fmt = argv[i] + 1; continue; }
        dprintf(STDERR_FILENO, "date: invalid date '%s'\n", argv[i]);
        return 1;
    }

    char buf[4096];
    if (fmt) {
        fmt_time(buf, sizeof buf, fmt, t, off, label);
        printf("%s\n", buf);
        return 0;
    }
    if (rfc_email) {
        fmt_time(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %z", t, off, label);
        printf("%s\n", buf);
        return 0;
    }
    if (rfc3339) {
        const char *f = strcmp(rfc3339, "date") == 0 ? "%Y-%m-%d"
                      : strcmp(rfc3339, "seconds") == 0 ? "%Y-%m-%d %H:%M:%S%:z"
                      : strcmp(rfc3339, "ns") == 0 ? "%Y-%m-%d %H:%M:%S.%N%:z"
                      : NULL;
        if (!f) {
            dprintf(STDERR_FILENO,
                    "date: invalid argument '%s' for '--rfc-3339'\n", rfc3339);
            return 1;
        }
        fmt_time(buf, sizeof buf, f, t, off, label);
        printf("%s\n", buf);
        return 0;
    }
    if (iso) {
        /* -Is and -Ised both mean seconds: GNU takes any unambiguous
         * prefix, and -Is is the form people actually type. */
        static const struct { const char *name, *fmt; } ISO[] = {
            { "hours",   "%Y-%m-%dT%H%:z" },
            { "minutes", "%Y-%m-%dT%H:%M%:z" },
            { "date",    "%Y-%m-%d" },
            { "seconds", "%Y-%m-%dT%H:%M:%S%:z" },
            { "ns",      "%Y-%m-%dT%H:%M:%S,%N%:z" },
            { NULL, NULL }
        };
        const char *f = NULL;
        size_t ilen = strlen(iso);
        for (int i = 0; ISO[i].name; i++)
            if (ilen && strncmp(ISO[i].name, iso, ilen) == 0) { f = ISO[i].fmt; break; }
        if (!f) {
            dprintf(STDERR_FILENO,
                    "date: invalid argument '%s' for '--iso-8601'\n"
                    "Valid arguments are:\n"
                    "  - 'hours'\n  - 'minutes'\n  - 'date'\n"
                    "  - 'seconds'\n  - 'ns'\n"
                    "Try 'date --help' for more information.\n", iso);
            return 1;
        }
        fmt_time(buf, sizeof buf, f, t, off, label);
        printf("%s\n", buf);
        return 0;
    }

    print_time(t, off, label);
    if (!when && !reffile) warn_if_unset();
    return 0;
}
