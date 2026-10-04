/* timezone - which time zone the clock is shown in.
 *
 *   timezone                    the zone now, the local time, the offset
 *   timezone list               the zones it knows by name
 *   timezone Asia/Seoul         set it (a city alone works too: seoul)
 *   timezone UTC+9              or an offset: +9, -5, +5:30, UTC
 *
 * ── Why this and not timedatectl ──
 *
 * The clock itself is always UTC; the zone is only how it is shown, and
 * the libc reads it from one place (tz.h). On the desktop that place is
 * a tzdata zone file and timedatectl writes it. A Pi or an iPhone's UTM
 * running LP-zero has neither: no /usr/share/zoneinfo (four megabytes on
 * a system that is thirteen) and no timedatectl, so `date -z Asia/Seoul`
 * said "cannot run timedatectl" and there was no way to leave UTC.
 *
 * The libc has always understood a second form for exactly this machine,
 * one line in /data/timezone:
 *
 *     540 KST                    minutes east of UTC, and the name
 *     -300 EST US EDT            with a summer-time rule and its name
 *
 * The rule is one of four (EU, US, AU, NZ), which between them cover the
 * zones below. What a line cannot carry is history - Seoul was +08:30 in
 * 1955 - so a file dated before a zone last changed its rules can show
 * an hour out. When zone files are there (the desktop, or tzdata), a
 * name is written instead and they are used, history and all.
 *
 * /data survives a reboot; the root filesystem does not, so it goes
 * there and nowhere else.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "tz.h"

#define SETTING "/data/timezone"

typedef struct {
    const char *name;     /* tzdata's name */
    int         minutes;  /* standard time, east of UTC */
    const char *std;      /* its abbreviation */
    const char *rule;     /* "", or EU / US / AU / NZ */
    const char *dst;      /* summer-time abbreviation */
} zone_t;

/* The rules as they stand. Mexico stopped summer time in 2022, Egypt went
 * back to it in 2023 on a rule none of the four is, so it is left out
 * rather than shown an hour wrong half the year. */
static const zone_t zones[] = {
    { "UTC",                            0,    "UTC",  "",   "" },
    { "Asia/Seoul",                     540,  "KST",  "",   "" },
    { "Asia/Tokyo",                     540,  "JST",  "",   "" },
    { "Asia/Shanghai",                  480,  "CST",  "",   "" },
    { "Asia/Hong_Kong",                 480,  "HKT",  "",   "" },
    { "Asia/Taipei",                    480,  "CST",  "",   "" },
    { "Asia/Singapore",                 480,  "+08",  "",   "" },
    { "Asia/Manila",                    480,  "PST",  "",   "" },
    { "Asia/Bangkok",                   420,  "+07",  "",   "" },
    { "Asia/Ho_Chi_Minh",               420,  "+07",  "",   "" },
    { "Asia/Jakarta",                   420,  "WIB",  "",   "" },
    { "Asia/Kolkata",                   330,  "IST",  "",   "" },
    { "Asia/Kathmandu",                 345,  "+0545","",   "" },
    { "Asia/Dhaka",                     360,  "+06",  "",   "" },
    { "Asia/Karachi",                   300,  "PKT",  "",   "" },
    { "Asia/Dubai",                     240,  "+04",  "",   "" },
    { "Asia/Riyadh",                    180,  "+03",  "",   "" },
    { "Europe/Istanbul",                180,  "+03",  "",   "" },
    { "Europe/Moscow",                  180,  "MSK",  "",   "" },
    { "Europe/London",                  0,    "GMT",  "EU", "BST" },
    { "Europe/Dublin",                  0,    "GMT",  "EU", "IST" },
    { "Europe/Lisbon",                  0,    "WET",  "EU", "WEST" },
    { "Europe/Paris",                   60,   "CET",  "EU", "CEST" },
    { "Europe/Berlin",                  60,   "CET",  "EU", "CEST" },
    { "Europe/Madrid",                  60,   "CET",  "EU", "CEST" },
    { "Europe/Rome",                    60,   "CET",  "EU", "CEST" },
    { "Europe/Amsterdam",               60,   "CET",  "EU", "CEST" },
    { "Europe/Stockholm",               60,   "CET",  "EU", "CEST" },
    { "Europe/Warsaw",                  60,   "CET",  "EU", "CEST" },
    { "Europe/Athens",                  120,  "EET",  "EU", "EEST" },
    { "Europe/Helsinki",                120,  "EET",  "EU", "EEST" },
    { "Europe/Kyiv",                    120,  "EET",  "EU", "EEST" },
    { "Africa/Johannesburg",            120,  "SAST", "",   "" },
    { "Africa/Lagos",                   60,   "WAT",  "",   "" },
    { "Africa/Nairobi",                 180,  "EAT",  "",   "" },
    { "America/New_York",               -300, "EST",  "US", "EDT" },
    { "America/Toronto",                -300, "EST",  "US", "EDT" },
    { "America/Chicago",                -360, "CST",  "US", "CDT" },
    { "America/Denver",                 -420, "MST",  "US", "MDT" },
    { "America/Phoenix",                -420, "MST",  "",   "" },
    { "America/Los_Angeles",            -480, "PST",  "US", "PDT" },
    { "America/Vancouver",              -480, "PST",  "US", "PDT" },
    { "America/Anchorage",              -540, "AKST", "US", "AKDT" },
    { "Pacific/Honolulu",               -600, "HST",  "",   "" },
    { "America/Mexico_City",            -360, "CST",  "",   "" },
    { "America/Bogota",                 -300, "-05",  "",   "" },
    { "America/Lima",                   -300, "-05",  "",   "" },
    { "America/Sao_Paulo",              -180, "-03",  "",   "" },
    { "America/Argentina/Buenos_Aires", -180, "-03",  "",   "" },
    { "Australia/Sydney",               600,  "AEST", "AU", "AEDT" },
    { "Australia/Melbourne",            600,  "AEST", "AU", "AEDT" },
    { "Australia/Brisbane",             600,  "AEST", "",   "" },
    { "Australia/Perth",                480,  "AWST", "",   "" },
    { "Pacific/Auckland",               720,  "NZST", "NZ", "NZDT" },
};
#define NZONES ((int)(sizeof zones / sizeof zones[0]))

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Equal ignoring case, with a space or '-' taken as the '_' tzdata uses. */
static bool same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = lower(*a), y = lower(*b);
        if (x == ' ' || x == '-') x = '_';
        if (y == ' ' || y == '-') y = '_';
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* "Asia/Seoul", "asia/seoul" or just "seoul" - the part after the last
 * slash, which is a city and unique in this table. */
static const zone_t *find(const char *want)
{
    for (int i = 0; i < NZONES; i++) {
        const char *city = strrchr(zones[i].name, '/');
        city = city ? city + 1 : zones[i].name;
        if (same(want, zones[i].name) || same(want, city))
            return &zones[i];
    }
    return NULL;
}

/* "+9", "-5", "+5:30", "UTC+9", "GMT-3:30", "UTC" -> minutes east.
 * false when it is not an offset at all. */
static bool parse_offset(const char *s, int *minutes)
{
    if (same(s, "utc") || same(s, "gmt") || same(s, "z")) {
        *minutes = 0;
        return true;
    }
    if ((lower(s[0]) == 'u' && lower(s[1]) == 't' && lower(s[2]) == 'c') ||
        (lower(s[0]) == 'g' && lower(s[1]) == 'm' && lower(s[2]) == 't'))
        s += 3;
    int sign;
    if (*s == '+') sign = 1;
    else if (*s == '-') sign = -1;
    else return false;
    s++;
    int h = 0, m = 0, digits = 0;
    while (*s >= '0' && *s <= '9') { h = h * 10 + (*s++ - '0'); digits++; }
    if (digits == 0 || digits > 2) return false;
    if (*s == ':') {
        s++;
        if (!(s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9')) return false;
        m = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
    }
    if (*s || h > 14 || m > 59) return false;
    *minutes = sign * (h * 60 + m);
    return true;
}

static bool write_setting(const char *line)
{
    long fd = lp_open(SETTING ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t n = strlen(line);
    bool ok = lp_write((int)fd, line, n) == (long)n && lp_write((int)fd, "\n", 1) == 1;
    lp_close((int)fd);
    if (!ok || lp_rename(SETTING ".new", SETTING) < 0) {
        lp_unlink(SETTING ".new");
        return false;
    }
    return true;
}

static int show(void)
{
    lp_tz_forget();
    s64 now = lp_time();
    lp_tm_t tm;
    lp_localtime(now, &tm);
    char when[64], off[16];
    lp_strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %a", &tm);
    lp_strftime(off, sizeof off, "%:z", &tm);
    const char *name = lp_tz_name();
    printf("zone        %s\n", name[0] ? name : (tm.zone[0] ? tm.zone : "UTC"));
    printf("local time  %s %s (UTC%s)\n", when, tm.zone[0] ? tm.zone : "UTC", off);
    s64 next;
    if (lp_tz_next_change(now, &next)) {
        lp_tm_t nt;
        lp_localtime(next, &nt);
        char nx[64];
        lp_strftime(nx, sizeof nx, "%Y-%m-%d %H:%M %Z", &nt);
        printf("next change %s\n", nx);
    }
    const char *src = lp_tz_source();
    printf("set in      %s\n", src[0] ? src : "nowhere - UTC");
    return 0;
}

static void usage(void)
{
    printf("Usage: timezone [list | ZONE | OFFSET]\n"
           "Show or set the time zone the clock is shown in.\n\n"
           "  timezone              the zone, the local time and the offset\n"
           "  timezone list         the zones known by name\n"
           "  timezone Asia/Seoul   set it - a city alone works: seoul, tokyo\n"
           "  timezone UTC+9        or an offset: +9, -5, +5:30, UTC\n\n"
           "Kept in %s, which survives a reboot. The clock itself\n"
           "stays UTC; `date -s` sets it.\n", SETTING);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return show();
    const char *a = argv[1];
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(a, "list") == 0 || strcmp(a, "-l") == 0) {
        for (int i = 0; i < NZONES; i++) {
            int m = zones[i].minutes, am = m < 0 ? -m : m;
            printf("  %-32s UTC%c%02d:%02d  %s%s%s\n", zones[i].name,
                   m < 0 ? '-' : '+', am / 60, am % 60, zones[i].std,
                   zones[i].dst[0] ? " / summer " : "", zones[i].dst);
        }
        printf("\nAny other zone as an offset: timezone UTC+5:30\n");
        return 0;
    }
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "timezone: only root can change the zone (sudo timezone %s)\n", a);
        return 1;
    }

    char line[96];
    const zone_t *z = find(a);
    int minutes;
    if (z && lp_tz_valid(z->name)) {
        /* Zone files are here: the name, and they bring the history. */
        strlcpy(line, z->name, sizeof line);
    } else if (z) {
        snprintf(line, sizeof line, "%d %s%s%s%s%s", z->minutes, z->std,
                 z->rule[0] ? " " : "", z->rule, z->dst[0] ? " " : "", z->dst);
    } else if (lp_tz_valid(a)) {
        strlcpy(line, a, sizeof line);
    } else if (parse_offset(a, &minutes)) {
        int am = minutes < 0 ? -minutes : minutes;
        if (am % 60)
            snprintf(line, sizeof line, "%d %c%02d%02d", minutes,
                     minutes < 0 ? '-' : '+', am / 60, am % 60);
        else if (minutes == 0)
            snprintf(line, sizeof line, "0 UTC");
        else
            snprintf(line, sizeof line, "%d %c%02d", minutes,
                     minutes < 0 ? '-' : '+', am / 60);
    } else {
        dprintf(STDERR_FILENO,
                "timezone: %s is not a zone this knows.\n"
                "timezone:   `timezone list` shows them; an offset works for any\n"
                "timezone:   other: timezone UTC+5:30\n", a);
        return 1;
    }
    if (!write_setting(line)) {
        dprintf(STDERR_FILENO, "timezone: cannot write %s - is /data mounted?\n", SETTING);
        return 1;
    }
    return show();
}
