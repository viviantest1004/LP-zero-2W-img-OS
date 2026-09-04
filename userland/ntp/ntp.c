/* ntp - set the system clock from the network.
 *
 *   ntp                  ask the default servers and set the clock
 *   ntp <server|IP>...   ask the servers you name
 *   ntp -r               no network: restore the last saved time
 *   ntp -d               daemon: save the time regularly and resync
 *
 * Why this is needed:
 *   The Pi Zero 2 W has no battery-backed clock. At power-on the kernel
 *   clock starts at 1 January 1970. Try HTTPS in that state and every
 *   server certificate looks like it starts in the future, so validation
 *   fails across the board. TLS needs the clock to be right.
 *
 * How:
 *   SNTP (RFC 4330). Send a 48-byte NTP packet to UDP 123 and read the
 *   transmit timestamp out of the reply. No precise synchronisation - no
 *   drift correction, no comparing servers. Seconds are enough for certs.
 *
 *   Host names are resolved here, by asking the nameserver in
 *   /etc/resolv.conf for an A record. Our libc has no resolver.
 *
 * The time we set is saved to /data/.clock, so the next boot can start
 * from it with -r even with no network. Far better than 1970.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* NTP counts from 1900-01-01, unix time from 1970-01-01.
 * This is the seconds between them: 70 years including 17 leap days. */
#define NTP_UNIX_DELTA  2208988800LL

#define NTP_PORT        123
#define DNS_PORT        53
#define CLOCK_FILE      "/data/.clock"

/* 2020-01-01. An answer earlier than this means something is wrong. */
#define SANITY_MIN      1577836800LL
/* 2100-01-01 */
#define SANITY_MAX      4102444800LL

static const char *DEFAULT_SERVERS[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
    "time.google.com",
    NULL
};

/* For the daemon: stay silent on failure. Spraying errors onto the
 * console every hour would make the serial console unusable. */
static bool quiet_mode = false;

/* ── Receive timeout ──────────────────────────────────────────────
 * Boot must not stall on a server that never answers. */
static void set_timeout(int fd, long seconds)
{
    /* struct __kernel_sock_timeval { s64 tv_sec; s64 tv_usec; } */
    s64 tv[2] = { seconds, 0 };
    lp_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));
}

/* Resolving a name is in the libc now - ntp was not the only thing that
 * needed it, and two copies of a DNS client is one too many. */

/* ── SNTP ───────────────────────────────────────────────────────
 * Unix seconds on success, 0 on failure. */
static s64 query_ntp(const char *server)
{
    u32 addr = net_resolve(server);
    if (addr == 0) {
        /* A name that will not resolve and a server that will not answer
         * have nothing in common. Say which, or nobody knows where to look. */
        if (!quiet_mode)
            dprintf(STDERR_FILENO, "ntp: %s: cannot resolve that name\n", server);
        return 0;
    }

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    set_timeout((int)fd, 3);

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port   = htons(NTP_PORT);
    to.sin_addr   = addr;

    /* 48 bytes, and only the first one has to be filled in:
     *   LI=0 (no warning) VN=4 (NTPv4) Mode=3 (client)
     *   0<<6 | 4<<3 | 3 = 0x23 */
    u8 pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;

    s64 result = 0;
    if (lp_sendto((int)fd, pkt, sizeof(pkt), 0, &to, sizeof(to)) > 0) {
        u8   resp[48];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);
        if (n < 0 && !quiet_mode) {
            char ip[16];
            ipv4_format(addr, ip);
            dprintf(STDERR_FILENO, "ntp: %s (%s): no reply\n", server, ip);
        }
        if (n == (long)sizeof(resp)) {
            /* transmit timestamp at offset 40; the high 4 bytes are seconds */
            u32 secs = ((u32)resp[40] << 24) | ((u32)resp[41] << 16) |
                       ((u32)resp[42] << 8)  |  (u32)resp[43];
            if (secs != 0)
                result = (s64)secs - NTP_UNIX_DELTA;
        }
    }

    lp_close((int)fd);
    return result;
}

static s64 query_ntp_quiet(const char *server)
{
    bool saved = quiet_mode;
    quiet_mode = true;
    s64 t = query_ntp(server);
    quiet_mode = saved;
    return t;
}

/* ── Save and restore ────────────────────────────────────────────
 * There is no RTC, so we stand in for one. Imperfect, but far better
 * than starting at 1970. */

static void save_clock(s64 t)
{
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);
    long fd = lp_open(CLOCK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;                      /* no /data: nothing to do */
    lp_write((int)fd, buf, (size_t)len);
    lp_close((int)fd);
}

static s64 load_clock(void)
{
    char buf[32];
    long fd = lp_open(CLOCK_FILE, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    /* Our libc has no atoll, and unix seconds are already 10 digits -
     * too many for a 32-bit atoi - so parse it here. */
    s64 v = 0;
    for (const char *c = buf; *c >= '0' && *c <= '9'; c++)
        v = v * 10 + (*c - '0');
    return v;
}

static void report(s64 t)
{
    lp_tm_t tm;
    lp_gmtime(t, &tm);
    printf("ntp: %d-%02d-%02d %02d:%02d:%02d UTC\n",
           tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
}

/* ── The daemon ──────────────────────────────────────────────────
 * This board has no battery-backed clock. Pull the power and time stops
 * dead - that is hardware, and no software can fix it.
 *
 * What we can do is write the time down often. Then the next boot picks
 * up from just before the power went. How often we save is the worst
 * case error: at every 30s, a sudden power cut costs under 30 seconds.
 *
 * The card's endurance is not a worry: 32 bytes every 30s is 92KB a day,
 * rewriting the same block, and wear levelling spreads that around.
 *
 * We also resync from the network hourly. Save-and-restore alone falls
 * behind by however long the machine was off. */
#define SAVE_EVERY_SEC   30
#define RETRY_AFTER_SEC   60   /* try again a minute after a failure */
#define RESYNC_EVERY_SEC 3600

static int run_daemon(const char **servers)
{
    printf("ntp: daemon started (saving every %ds, resyncing every %dmin)\n",
           SAVE_EVERY_SEC, RESYNC_EVERY_SEC / 60);

    s64 last_sync = 0;

    for (;;) {
        s64 now = lp_time();

        /* Only save a plausible time. Saving 1970 would send the next boot
         * back to it, which is worse than nothing. */
        if (now >= SANITY_MIN)
            save_clock(now);

        if (now - last_sync >= RESYNC_EVERY_SEC || last_sync == 0) {
            bool got_it = false;
            for (int i = 0; servers[i]; i++) {
                s64 t = query_ntp_quiet(servers[i]);
                if (t >= SANITY_MIN && t <= SANITY_MAX) {
                    if (lp_settime(t) == 0)
                        save_clock(t);
                    got_it = true;
                    break;
                }
            }

            /* A failed attempt must not count as a sync.
             *
             * last_sync was set either way, so the first try - which on
             * a first boot happens seconds after init starts, before
             * WiFi has associated - failed and then booked itself an
             * hour of silence. The board ran that whole hour believing
             * it was 1970: every HTTPS certificate outside its validity
             * window, pkg and python failing on dates, every log line
             * stamped wrong, and nothing said why.
             *
             * On failure, come back in a minute instead. */
            if (got_it)
                last_sync = lp_time();
            else
                last_sync = lp_time() - RESYNC_EVERY_SEC + RETRY_AFTER_SEC;
        }

        lp_sleep_ms(SAVE_EVERY_SEC * 1000);
    }
    return 0;   /* not reached */
}

int main(int argc, char **argv)
{
    /* -r: restore the saved time without a network. Used early in boot. */
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        s64 saved = load_clock();
        if (saved < SANITY_MIN) {
            dprintf(STDERR_FILENO, "ntp: no saved time\n");
            return 1;
        }
        /* Never move the clock backwards. */
        if (lp_time() >= saved) {
            printf("ntp: the clock is already ahead of that\n");
            return 0;
        }
        if (lp_settime(saved) < 0) {
            dprintf(STDERR_FILENO, "ntp: cannot set the clock (are you root?)\n");
            return 1;
        }
        printf("ntp: restored the saved time (not from the network)\n");
        report(saved);
        return 0;
    }

    bool daemon = false;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon = true;
        argv++;
        argc--;
    }

    const char **servers = DEFAULT_SERVERS;
    const char  *from_args[8];
    if (argc > 1) {
        int n = 0;
        for (int i = 1; i < argc && n < 7; i++)
            from_args[n++] = argv[i];
        from_args[n] = NULL;
        servers = from_args;
    }

    if (daemon)
        return run_daemon(servers);

    for (int i = 0; servers[i]; i++) {
        s64 t = query_ntp(servers[i]);
        if (t < SANITY_MIN || t > SANITY_MAX) {
            if (t != 0)
                dprintf(STDERR_FILENO, "ntp: %s gave an implausible answer\n", servers[i]);
            continue;
        }
        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO, "ntp: cannot set the clock (are you root?)\n");
            return 1;
        }
        printf("ntp: got the time from %s\n", servers[i]);
        report(t);
        save_clock(t);
        return 0;
    }

    dprintf(STDERR_FILENO,
            "ntp: could not get the time.\n"
            "     UDP 123 may be blocked (common on public and office networks),\n"
            "     or there may be no address yet. To use another server:\n"
            "       ntp <server>\n");
    return 1;
}
