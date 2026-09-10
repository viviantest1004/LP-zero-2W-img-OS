/* wifi - find out why the wireless will not connect, and try what works.
 *
 *   wifi              what the wireless is doing, and where it stopped
 *   wifi fix          try each known workaround in turn, say which works
 *   wifi fix -k       ... and keep the one that did
 *   wifi log          the lines of the last attempt that decide anything
 *
 * ── Why this exists ──
 * This board associates with the access point and then the WPA2
 * four-way handshake never starts: "Authentication with ... timed out",
 * then a disconnect wpa_supplicant generated itself. Everything that
 * could be checked from a distance has been: the firmware, the NVRAM,
 * the CLM blob, the kernel's wireless and crypto options against the
 * Raspberry Pi's own defconfig, and the wpa_supplicant build - the
 * four-way handshake code is in the binary and management frame
 * protection is off. None of it explains the failure.
 *
 * So this stops guessing from here and bisects on the machine that has
 * the problem. Each variant below is one thing that is known to matter
 * on a fullmac chip - where the firmware, not the host, does the
 * association - and each is tried for real, on the real radio, against
 * the real access point, with the answer being whether the handshake
 * completes. One of them working is a fix. All of them failing is also
 * an answer, and a much better one than another theory.
 *
 * ── What it will not do ──
 * Change anything without being asked. `wifi fix` puts the wireless
 * back exactly as it found it when it finishes, and only `-k` writes a
 * configuration - to /data/wpa_supplicant.conf, where the boot reads
 * it, and never to /boot, which belongs to whoever holds the card.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define IFACE       "wlan0"
#define WPA_LIVE    "/etc/wpa.conf"
#define WPA_DATA    "/data/wpa_supplicant.conf"
#define WPA_LOG     "/data/log/wpa.log"
#define MSG_LOG     "/data/log/messages"
#define TRY_CONF    "/tmp/wifi-try.conf"
#define CTRL_DIR    "/var/run/wpa_supplicant"

/* How long one attempt gets. An association plus a four-way handshake
 * is normally under three seconds; a scan first can take ten. Twenty-
 * five is generous enough that a slow answer is not called a failure,
 * and seven variants still finish in under four minutes. */
#define TRY_SECS    25

/* ── running things ──────────────────────────────────────────────── */

/* Run a program and collect what it prints. Read before waiting: a
 * child that fills the pipe blocks on write, and a parent that waits
 * first would then never read - the two of them stuck on each other. */
static int run_capture(const char *path, char *const argv[],
                       char *out, size_t size)
{
    if (out && size) out[0] = '\0';

    int fds[2];
    if (lp_pipe(fds) < 0)
        return -1;

    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(fds[0]); lp_close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        lp_close(fds[0]);
        lp_dup2(fds[1], STDOUT_FILENO);
        lp_dup2(fds[1], STDERR_FILENO);
        lp_close(fds[1]);
        lp_execve(path, argv, environ);
        lp_exit(127);
    }

    lp_close(fds[1]);
    size_t got = 0;
    for (;;) {
        if (!out || got + 1 >= size) {
            char sink[256];
            if (lp_read(fds[0], sink, sizeof sink) <= 0) break;
            continue;
        }
        long n = lp_read(fds[0], out + got, size - got - 1);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (out && size) out[got] = '\0';
    lp_close(fds[0]);

    int status = 0;
    lp_waitpid(pid, &status, 0);
    return (status & 0x7f) ? -1 : ((status >> 8) & 0xff);
}

static int run_quiet(const char *path, char *const argv[])
{
    return run_capture(path, argv, NULL, 0);
}

/* ── asking wpa_supplicant ───────────────────────────────────────── */

/* One field out of `wpa_cli status`, which prints "key=value" lines. */
static bool wpa_field(const char *key, char *out, size_t n)
{
    char buf[4096];
    char *argv[] = { (char *)"wpa_cli", (char *)"-i", (char *)IFACE,
                     (char *)"status", NULL };
    if (run_capture("/bin/wpa_cli", argv, buf, sizeof buf) < 0)
        return false;

    size_t klen = strlen(key);
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            strlcpy(out, p + klen + 1, n);
            return out[0] != '\0';
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

static bool have_iface(void)
{
    char path[64];
    snprintf(path, sizeof path, "/sys/class/net/%s", IFACE);
    return lp_exists(path);
}

/* ── the configuration we are testing with ───────────────────────── */

/* Pull ssid= and psk= out of whatever configuration this machine is
 * already using. Everything below reuses them: asking somebody to type
 * their password again to run a diagnosis is how a diagnosis does not
 * get run. */
static bool read_creds(char *ssid, size_t sn, char *psk, size_t pn)
{
    ssid[0] = psk[0] = '\0';

    const char *files[] = { WPA_LIVE, WPA_DATA, "/boot/wpa_supplicant.conf",
                            NULL };
    for (int i = 0; files[i]; i++) {
        long fd = lp_open(files[i], O_RDONLY, 0);
        if (fd < 0)
            continue;
        char line[512];
        while (readline((int)fd, line, sizeof line) >= 0) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "ssid=", 5) == 0)
                strlcpy(ssid, p + 5, sn);
            else if (strncmp(p, "psk=", 4) == 0)
                strlcpy(psk, p + 4, pn);
        }
        lp_close((int)fd);
        if (ssid[0])
            return true;
    }
    return false;
}

/* ── the variants ────────────────────────────────────────────────── */

typedef struct {
    const char *name;       /* what to call it in the report          */
    const char *driver;     /* -D                                     */
    const char *global;     /* extra lines above the network block    */
    const char *net;        /* extra lines inside the network block   */
    const char *why;        /* what this one is testing               */
} variant_t;

static const variant_t VARIANTS[] = {
    { "as configured", "nl80211", "", "",
      "what the machine does today, so the rest have something to beat" },

    { "no management frame protection", "nl80211", "", "    ieee80211w=0\n",
      "some fullmac firmware advertises 802.11w and then will not finish"
      " the handshake with it" },

    { "CCMP only", "nl80211", "",
      "    proto=RSN\n    pairwise=CCMP\n    group=CCMP\n",
      "with the defaults the supplicant offers TKIP as well, and a chip"
      " that picks it for the group key can stall here" },

    { "let the driver choose", "nl80211", "ap_scan=2\n", "",
      "ap_scan=2 hands the association to the firmware instead of"
      " driving it from scan results - the oldest fullmac workaround"
      " there is" },

    { "ask for the network by name", "nl80211", "", "    scan_ssid=1\n",
      "a directed probe rather than waiting to see the network in a"
      " broadcast scan" },

    { "the other driver backend", "wext", "", "",
      "wext and nl80211 reach the same brcmfmac through different"
      " kernel code, so a bug in one is not a bug in the other" },

    { "the other backend, CCMP only", "wext", "",
      "    proto=RSN\n    pairwise=CCMP\n    group=CCMP\n",
      "the two most likely fixes together, in case neither is enough"
      " alone" },
};

#define NVARIANTS ((int)(sizeof VARIANTS / sizeof VARIANTS[0]))

static bool write_try_conf(const variant_t *v, const char *ssid,
                           const char *psk)
{
    long fd = lp_open(TRY_CONF, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "wifi: cannot write %s (%ld)\n",
                TRY_CONF, -fd);
        return false;
    }
    dprintf((int)fd, "ctrl_interface=%s\n", CTRL_DIR);
    if (v->global[0])
        dprintf((int)fd, "%s", v->global);
    dprintf((int)fd, "network={\n    ssid=%s\n    psk=%s\n"
                     "    key_mgmt=WPA-PSK\n", ssid, psk);
    if (v->net[0])
        dprintf((int)fd, "%s", v->net);
    dprintf((int)fd, "}\n");
    lp_close((int)fd);
    return true;
}

/* ── stopping and starting the real one ──────────────────────────── */

static void service_do(const char *verb)
{
    char *argv[] = { (char *)"service", (char *)verb,
                     (char *)"wpa_supplicant", NULL };
    run_quiet("/bin/service", argv);
}

/* Kill anything still holding the radio. `service stop` deals with the
 * supervised one; a wpa_supplicant left behind by an earlier run of
 * this command would otherwise own the control socket and every
 * variant after it would be measuring the wrong process. */
static void kill_strays(void)
{
    char out[512];
    char *argv[] = { (char *)"pidof", (char *)"wpa_supplicant", NULL };
    if (run_capture("/bin/pidof", argv, out, sizeof out) != 0)
        return;
    for (char *p = out; *p; ) {
        while (*p == ' ' || *p == '\n') p++;
        if (!*p) break;
        long pid = strtol(p, &p, 10);
        if (pid > 1)
            lp_kill((pid_t)pid, 15);
    }
    lp_sleep_ms(500);
}

/* ── one attempt ─────────────────────────────────────────────────── */

/* Returns 1 connected, 0 did not, -1 could not even start. `state_out`
 * gets the last state seen, which is the whole diagnosis when it fails:
 * SCANNING means it never found the network, ASSOCIATED means it got
 * on and the handshake did not happen, 4WAY_HANDSHAKE means it started
 * and did not finish. */
static int try_variant(const variant_t *v, const char *ssid, const char *psk,
                       char *state_out, size_t sn)
{
    strlcpy(state_out, "-", sn);

    if (!write_try_conf(v, ssid, psk))
        return -1;

    kill_strays();

    char *argv[] = { (char *)"wpa_supplicant", (char *)"-B",
                     (char *)"-i", (char *)IFACE,
                     (char *)"-c", (char *)TRY_CONF,
                     (char *)"-D", (char *)v->driver,
                     (char *)"-f", (char *)WPA_LOG, (char *)"-t",
                     NULL };
    char out[1024];
    int rc = run_capture("/bin/wpa_supplicant", argv, out, sizeof out);
    if (rc != 0) {
        /* Its own first line says why - an unsupported driver, a
         * configuration it would not read - and that is worth more
         * than our summary of it. */
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
        if (out[0])
            strlcpy(state_out, out, sn);
        else
            strlcpy(state_out, "would not start", sn);
        return -1;
    }

    for (int t = 0; t < TRY_SECS * 2; t++) {
        lp_sleep_ms(500);
        char st[64];
        if (!wpa_field("wpa_state", st, sizeof st))
            continue;
        strlcpy(state_out, st, sn);
        if (strcmp(st, "COMPLETED") == 0)
            return 1;
    }
    return 0;
}

/* ── the report ──────────────────────────────────────────────────── */

static void show_line(const char *label, const char *value)
{
    printf("  %-14s %s\n", label, value);
}

static void show_status(void)
{
    printf("\n");

    if (!have_iface()) {
        show_line("wireless", "there is no wlan0 on this machine");
        printf("\n  A virtual machine has no radio, and neither does a"
               " board whose\n  firmware did not load. `dmesg | grep"
               " brcmfmac` says which.\n\n");
        return;
    }
    show_line("interface", IFACE);

    /* Which chip, from the driver that claimed it. */
    char link[256], path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", IFACE);
    long n = lp_readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = '\0';
        const char *slash = strrchr(link, '/');
        show_line("driver", slash ? slash + 1 : link);
    }

    char v[128];
    if (wpa_field("wpa_state", v, sizeof v)) {
        show_line("state", v);
        if (strcmp(v, "COMPLETED") == 0) {
            char s[128];
            if (wpa_field("ssid", s, sizeof s))    show_line("network", s);
            if (wpa_field("ip_address", s, sizeof s)) show_line("address", s);
            printf("\n  The wireless is up.\n\n");
            return;
        }
        if (wpa_field("ssid", v, sizeof v))  show_line("network", v);
        if (wpa_field("bssid", v, sizeof v)) show_line("access point", v);
    } else {
        show_line("state", "wpa_supplicant is not answering"
                           " (`service status wpa_supplicant`)");
    }

    /* The lines that decide what went wrong. Reading a megabyte of
     * association log to find them is exactly what nobody does. */
    printf("\n  the last attempt:\n");
    const char *files[] = { WPA_LOG, MSG_LOG, NULL };
    static const char *KEY[] = {
        "Trying to associate", "Associated with", "RX message 1 of 4-Way",
        "Key negotiation completed", "CTRL-EVENT-CONNECTED",
        "timed out", "CTRL-EVENT-DISCONNECTED", "CTRL-EVENT-ASSOC-REJECT",
        "WRONG_KEY", "pre-shared key may be incorrect", NULL
    };
    int shown = 0;
    for (int f = 0; files[f] && shown < 12; f++) {
        long fd = lp_open(files[f], O_RDONLY, 0);
        if (fd < 0) continue;
        char line[512];
        while (readline((int)fd, line, sizeof line) >= 0 && shown < 12)
            for (int k = 0; KEY[k]; k++)
                if (strstr(line, KEY[k])) {
                    printf("    %s\n", line);
                    shown++;
                    break;
                }
        lp_close((int)fd);
    }
    if (!shown)
        printf("    nothing recorded. `touch /boot/wpa-debug` on the card"
               " and reboot -\n    the next attempt is written to %s in"
               " full.\n", WPA_LOG);

    printf("\n  `wifi fix` tries each thing that is known to matter on"
           " this chip,\n  one at a time, and says which one works.\n\n");
}

/* ── fix ─────────────────────────────────────────────────────────── */

static int do_fix(bool keep)
{
    if (!have_iface()) {
        dprintf(STDERR_FILENO,
                "wifi: there is no %s on this machine - nothing to fix\n",
                IFACE);
        return 1;
    }

    char ssid[128], psk[256];
    if (!read_creds(ssid, sizeof ssid, psk, sizeof psk)) {
        dprintf(STDERR_FILENO,
                "wifi: no network is configured yet, so there is nothing"
                " to test with.\n"
                "wifi:   `net wifi <name> <password>` first.\n");
        return 1;
    }

    printf("\n  network      %s\n", ssid);
    printf("  each attempt gets %d seconds. Seven of them, so about four"
           " minutes.\n", TRY_SECS);
    printf("  the wireless is put back the way it was at the end,"
           " whatever happens.\n\n");

    service_do("stop");

    int winner = -1;
    char state[NVARIANTS][64];

    for (int i = 0; i < NVARIANTS; i++) {
        printf("  %d/%d  %-32s ", i + 1, NVARIANTS, VARIANTS[i].name);
        int r = try_variant(&VARIANTS[i], ssid, psk,
                            state[i], sizeof state[0]);
        if (r == 1) {
            printf("CONNECTED\n");
            winner = i;
            break;
        }
        printf("%s\n", state[i][0] ? state[i] : "no");
    }

    kill_strays();
    lp_unlink(TRY_CONF);

    printf("\n");
    if (winner < 0) {
        printf("  None of them connected.\n\n");
        printf("  That is worth knowing: it is not the cipher, not"
               " management frame\n"
               "  protection, not the scan, and not the driver backend."
               " The state each\n"
               "  one reached is above - if they all say ASSOCIATED, the"
               " access point\n"
               "  answers and the handshake never starts, and the next"
               " thing to read is\n"
               "  the full log: `touch /boot/wpa-debug` on the card,"
               " reboot, then\n"
               "  %s.\n\n", WPA_LOG);
        service_do("start");
        return 1;
    }

    printf("  %s works.\n", VARIANTS[winner].name);
    printf("    %s\n\n", VARIANTS[winner].why);

    if (!keep) {
        printf("  Nothing was changed. `wifi fix -k` does the same and"
               " keeps it.\n\n");
        service_do("start");
        return 0;
    }

    /* Keep it where the boot reads it, and nowhere else. /boot belongs
     * to whoever holds the card and is the source of truth while the
     * file is there; writing behind that would make the next boot undo
     * this and leave nobody able to explain why. */
    if (!write_try_conf(&VARIANTS[winner], ssid, psk)) {
        service_do("start");
        return 1;
    }
    char *cp[] = { (char *)"cp", (char *)TRY_CONF, (char *)WPA_DATA, NULL };
    if (run_quiet("/bin/cp", cp) != 0) {
        dprintf(STDERR_FILENO, "wifi: could not write %s\n", WPA_DATA);
        service_do("start");
        return 1;
    }
    lp_chmod(WPA_DATA, 0600);
    lp_unlink(TRY_CONF);

    printf("  Kept in %s.\n", WPA_DATA);
    if (lp_exists("/boot/wpa_supplicant.conf"))
        printf("  Delete /boot/wpa_supplicant.conf from the card, or the"
               " next boot\n  copies it back over this.\n");
    if (strcmp(VARIANTS[winner].driver, "nl80211") != 0)
        printf("  The driver backend is not in that file. Change"
               " /etc/services to say\n  `-D %s`, or run `wifi fix -k`"
               " again after a reboot.\n",
               VARIANTS[winner].driver);
    printf("\n");

    service_do("start");
    return 0;
}

/* ── log ─────────────────────────────────────────────────────────── */

static int do_log(void)
{
    if (!lp_exists(WPA_LOG)) {
        printf("\n  There is no %s.\n\n"
               "  It is written only when the switch is on:"
               " `touch /boot/wpa-debug`\n"
               "  on the card - any PC can, it is FAT32 - and the next"
               " boot records\n"
               "  every frame of the association and the handshake.\n\n",
               WPA_LOG);
        return 1;
    }
    char *argv[] = { (char *)"more", (char *)WPA_LOG, NULL };
    lp_execve("/bin/more", argv, environ);
    dprintf(STDERR_FILENO, "wifi: cannot run more\n");
    return 1;
}

static void usage(void)
{
    printf("usage: wifi [fix [-k] | log]\n"
           "\n"
           "  wifi          what the wireless is doing, and where it"
           " stopped\n"
           "  wifi fix      try each known workaround, say which works\n"
           "  wifi fix -k   ... and keep the one that did\n"
           "  wifi log      the last association, frame by frame\n");
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        show_status();
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "fix") == 0) {
        bool keep = (argc > 2 && strcmp(argv[2], "-k") == 0);
        if (argc > 2 && !keep) {
            dprintf(STDERR_FILENO, "wifi: %s is not an option\n", argv[2]);
            usage();
            return 2;
        }
        return do_fix(keep);
    }
    if (strcmp(argv[1], "log") == 0)
        return do_log();

    dprintf(STDERR_FILENO, "wifi: %s is not a command\n", argv[1]);
    usage();
    return 2;
}
