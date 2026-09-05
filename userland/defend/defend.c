/* defend - the security checks that are worth running on this machine.
 *
 *   defend                 run every check once and report
 *   defend -d              run as a service, checking periodically
 *   defend -i <seconds>    how often -d checks (default 300)
 *   defend -a              include /data/debian and /data/python in the walk
 *   defend -n              say what would be banned, ban nothing
 *   defend status          what it has seen
 *   defend unban <addr>    let an address back in
 *   defend baseline        record the current state as expected
 *
 * ── This is not an antivirus, and it should not be ──
 * The owner asked for antivirus. A signature scanner would be dishonest
 * here: the databases are tens of megabytes against a whole operating
 * system of 13-22MB, they are worthless the week they stop being
 * updated, and this board may sit on a wall for months with nobody
 * feeding it anything. A scanner that has not been updated since the
 * image was built finds 2019's malware and nothing since, while
 * printing "no threats found" - which is worse than printing nothing,
 * because somebody believes it.
 *
 * What actually happens to a small internet-facing Linux board is a
 * short list, and every item on it is observable without a database:
 *
 *   1  somebody works through passwords on port 22
 *   2  a file appears under /data that has no business being there -
 *      setuid, world-writable, owned by nobody, or executable in a
 *      place that holds data
 *   3  something starts listening on a port that was not open before
 *   4  one of the few files that survive a reboot is edited, so that
 *      whatever got in gets in again
 *   5  a line is added to authorized_keys
 *
 * Five checks, no database, no updates, and each one names the file,
 * the address or the port it is talking about and the command that
 * deals with it. That is the whole design.
 *
 * ── Where the blocking happens, and why it is a second nftables table ──
 * firewall(1) owns table "lpzero" and rebuilds it whole on every apply -
 * that is what makes `firewall on` idempotent and atomic. A rule that
 * defend appended to that table would therefore vanish the next time
 * anybody ran `firewall allow 8080`, silently, and the bans would be
 * gone with no message anywhere. firewall also has no verb that takes
 * an address; it deals in ports.
 *
 * So defend owns its own table, "lpdefend", with one input chain at
 * priority -10 - ahead of firewall's chain at 0 - whose policy is
 * accept. A packet from a banned address is dropped there; everything
 * else falls straight through to the firewall's own chain, which is
 * still the thing deciding what is open. The two never touch each
 * other's rules, either can be rebuilt without the other noticing, and
 * `firewall off` does not unban anybody.
 *
 * The netlink shape below is deliberately the same as firewall.c's, so
 * that reading one teaches you the other. It is much smaller: one
 * table, one chain, and every rule is "source address is X, drop".
 *
 * ── State ──
 * /data/defend/bans       one address per line, with when and why
 * /data/defend/baseline   listeners and SSH keys, as they should be
 * /data/defend/state      counters, so `defend status` has a history
 *
 * The kernel keeps no nftables table across a reboot, so the bans are
 * re-applied from the file at every start. That is the reason the file
 * is the truth and the table is only its shadow.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* ── where everything lives ──────────────────────────────────────── */

#define DIR_STATE   "/data/defend"
#define F_BANS      DIR_STATE "/bans"
#define F_BASELINE  DIR_STATE "/baseline"
#define F_STATE     DIR_STATE "/state"
#define AUTH_LOG    "/data/log/auth"

/* Thresholds. Deliberately few, and none of them configurable: a knob
 * nobody turns is a knob that only ever gets set wrong. */
#define WINDOW_SECS   600      /* how far back a failure still counts   */
#define FAIL_LIMIT      8      /* failures in that window -> banned     */
#define BAN_SECS  (7*24*3600)  /* how long a ban lasts                  */
#define MAX_BANS      128
#define MAX_SOURCES    64

#define DEFAULT_INTERVAL 300   /* seconds between checks under -d       */
#define FULL_EVERY        12   /* ... and every 12th one is a full scan */

/* getdents64 records: reclen at byte 16, the name at 19 (as ls does). */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

static bool opt_daemon;
static bool opt_all;          /* -a: walk the big trees too */
static bool opt_dry;          /* -n: report, ban nothing    */
static int  interval = DEFAULT_INTERVAL;

/* ── saying things ───────────────────────────────────────────────── */

static int findings;

/* Every finding goes through here, so that all of them are counted the
 * same way and all of them reach the log when this runs as a service.
 * The console scrolls and is gone; /data/log/messages is not. */
static void report(const char *text)
{
    findings++;
    printf("defend: ** %s\n", text);
    if (opt_daemon)
        lp_log("defend", text);
}

/* ── /proc/net address parsing ───────────────────────────────────────
 *
 * /proc/net/tcp prints an address as eight hex digits of a u32 held in
 * the HOST's byte order. Both architectures here are little-endian, so
 * the bytes of that u32 are already in network order - which is what
 * ipv4_format wants - and nothing is swapped anywhere below. Get this
 * wrong and there is no error, only 1.0.0.127 where 127.0.0.1 belongs.
 * netstat.c and route.c have done it this way since they were written.
 *
 * The port after the colon is plain hex, no byte order involved. */

static u32 hexn(const char *s, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        u32  d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A') + 10;
        else break;
        v = v * 16 + d;
    }
    return v;
}

/* Split a line on runs of spaces and tabs, in place. */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* "0100007F:0016" or the 32-digit v6 form, if it is IPv4 or an IPv4
 * address the kernel filed under tcp6 as ::ffff:a.b.c.d. false for a
 * real IPv6 address - the caller decides what to do about that. */
static bool hex_v4(const char *s, u32 *be, u16 *port)
{
    const char *colon = strchr(s, ':');
    if (!colon)
        return false;
    int digits = (int)(colon - s);
    if (port)
        *port = (u16)hexn(colon + 1, 4);

    if (digits == 8) { *be = hexn(s, 8); return true; }
    if (digits != 32) return false;

    u32 w[4];
    u8  b[16];
    for (int i = 0; i < 4; i++) {
        w[i] = hexn(s + i * 8, 8);
        memcpy(b + i * 4, &w[i], 4);
    }
    for (int i = 0; i < 10; i++)
        if (b[i]) return false;
    if (b[10] != 0xFF || b[11] != 0xFF)
        return false;
    *be = w[3];
    return true;
}

/* One IPv6 group, lower case, no leading zeros. */
static int put_hex16(char *out, u16 v)
{
    static const char d[] = "0123456789abcdef";
    char tmp[4];
    int  n = 0;

    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = d[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* The longest run of zero groups collapses to "::". Without it a
 * socket listening on every address prints as 0:0:0:0:0:0:0:0 and the
 * line is wider than the screen. */
static void fmt_v6(const u8 *b, char *out)
{
    u16 g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (u16)((b[2 * i] << 8) | b[2 * i + 1]);

    int best = -1, best_len = 0;
    for (int i = 0; i < 8; ) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > best_len) { best_len = j - i; best = i; }
        i = j;
    }
    if (best_len < 2)
        best = -1;

    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + best_len) {
            if (i == best) out[o++] = ':';
            continue;
        }
        if (i > 0) out[o++] = ':';
        o += put_hex16(out + o, g[i]);
    }
    if (best >= 0 && best + best_len == 8)
        out[o++] = ':';
    out[o] = '\0';
}

/* "XXXXXXXX:PPPP" straight out of /proc/net -> "1.2.3.4:22". */
static void fmt_sock(const char *s, char *out, size_t cap)
{
    u32 be; u16 port;

    if (hex_v4(s, &be, &port)) {
        char host[20];
        ipv4_format(be, host);
        snprintf(out, cap, "%s:%u", host, (u32)port);
        return;
    }

    const char *colon = strchr(s, ':');
    if (!colon || colon - s != 32) { strlcpy(out, s, cap); return; }

    u8 b[16];
    for (int i = 0; i < 4; i++) {
        u32 w = hexn(s + i * 8, 8);
        memcpy(b + i * 4, &w, 4);
    }
    char host[46];
    fmt_v6(b, host);
    snprintf(out, cap, "[%s]:%u", host, (u32)hexn(colon + 1, 4));
}

/* ── nftables, for the ban list ──────────────────────────────────── */

#define AF_NETLINK          16
#define NETLINK_NETFILTER   12

#define NLM_F_REQUEST   0x001
#define NLM_F_ACK       0x004
#define NLM_F_CREATE    0x400
#define NLM_F_APPEND    0x800

#define NLMSG_ERROR     2
#define NLMSG_DONE      3
#define NLA_F_NESTED    0x8000

typedef struct {
    u32 nlmsg_len;
    u16 nlmsg_type;
    u16 nlmsg_flags;
    u32 nlmsg_seq;
    u32 nlmsg_pid;
} nlmsghdr_t;

typedef struct { u16 nla_len; u16 nla_type; } nlattr_t;

typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

typedef struct { u8 nfgen_family; u8 version; u16 res_id; } nfgenmsg_t;

#define NFNL_SUBSYS_NFTABLES  10
#define NFNL_MSG_BATCH_BEGIN  16
#define NFNL_MSG_BATCH_END    17

#define NFT_MSG_NEWTABLE   0
#define NFT_MSG_GETTABLE   1
#define NFT_MSG_DELTABLE   2
#define NFT_MSG_NEWCHAIN   3
#define NFT_MSG_NEWRULE    6

#define NFT(m)  ((u16)((NFNL_SUBSYS_NFTABLES << 8) | (m)))

#define NFPROTO_INET   1
#define NFPROTO_IPV4   2

#define NFTA_LIST_ELEM        1
#define NFTA_TABLE_NAME       1
#define NFTA_CHAIN_TABLE      1
#define NFTA_CHAIN_NAME       3
#define NFTA_CHAIN_HOOK       4
#define NFTA_CHAIN_POLICY     5
#define NFTA_CHAIN_TYPE       7
#define NFTA_HOOK_HOOKNUM     1
#define NFTA_HOOK_PRIORITY    2
#define NFTA_RULE_TABLE       1
#define NFTA_RULE_CHAIN       2
#define NFTA_RULE_EXPRESSIONS 4
#define NFTA_RULE_USERDATA    7
#define NFTA_EXPR_NAME        1
#define NFTA_EXPR_DATA        2
#define NFTA_DATA_VALUE       1
#define NFTA_DATA_VERDICT     2
#define NFTA_VERDICT_CODE     1
#define NFTA_META_DREG        1
#define NFTA_META_KEY         2
#define NFTA_PAYLOAD_DREG     1
#define NFTA_PAYLOAD_BASE     2
#define NFTA_PAYLOAD_OFFSET   3
#define NFTA_PAYLOAD_LEN      4
#define NFTA_CMP_SREG         1
#define NFTA_CMP_OP           2
#define NFTA_CMP_DATA         3
#define NFTA_IMMEDIATE_DREG   1
#define NFTA_IMMEDIATE_DATA   2

#define NFT_REG_VERDICT   0
#define NFT_REG_1         1
#define NF_DROP           0
#define NF_ACCEPT         1

#define NFT_META_NFPROTO  15
#define NFT_PAYLOAD_NETWORK_HEADER 1
#define NFT_CMP_EQ        0
#define NF_INET_LOCAL_IN  1

#define TABLE_NAME  "lpdefend"
#define CHAIN_NAME  "banned"

/* Ahead of firewall's chain, which sits at 0 where iptables' filter is.
 * A banned packet is dropped before the firewall ever considers it, and
 * a packet from anybody else falls through untouched. */
#define BAN_PRIORITY  ((u32)(-10))

#define BATCH_MAX  16384

typedef struct {
    u8   buf[BATCH_MAX];
    u32  len;
    u32  msg_start;
    u32  nest[8];
    int  depth;
    u32  seq;
    bool overflow;
} batch_t;

static u32 align4(u32 n) { return (n + 3u) & ~3u; }

static void *reserve(batch_t *b, u32 n)
{
    static u8 sink[256];
    if (b->len + n > BATCH_MAX) { b->overflow = true; return sink; }
    u8 *p = b->buf + b->len;
    memset(p, 0, n);
    b->len += n;
    return p;
}

static void pad4(batch_t *b)
{
    while (b->len & 3u) {
        if (b->len >= BATCH_MAX) { b->overflow = true; return; }
        b->buf[b->len++] = 0;
    }
}

static void msg_begin(batch_t *b, u16 type, u16 flags)
{
    pad4(b);
    b->msg_start = b->len;
    nlmsghdr_t *h = reserve(b, sizeof *h);
    h->nlmsg_type  = type;
    h->nlmsg_flags = flags;
    h->nlmsg_seq   = ++b->seq;
    h->nlmsg_pid   = 0;
    nfgenmsg_t *g = reserve(b, sizeof *g);
    g->nfgen_family = NFPROTO_INET;
    g->version      = 0;
    g->res_id       = htons(0);
}

static void msg_end(batch_t *b)
{
    if (b->overflow) return;
    nlmsghdr_t *h = (nlmsghdr_t *)(b->buf + b->msg_start);
    h->nlmsg_len = b->len - b->msg_start;
    pad4(b);
}

static void attr_put(batch_t *b, u16 type, const void *data, u16 len)
{
    nlattr_t *a = reserve(b, sizeof *a);
    a->nla_type = type;
    a->nla_len  = (u16)(sizeof *a + len);
    if (len) memcpy(reserve(b, len), data, len);
    pad4(b);
}

static void attr_u32(batch_t *b, u16 type, u32 v)
{
    u32 be = htonl(v);
    attr_put(b, type, &be, 4);
}

static void attr_str(batch_t *b, u16 type, const char *s)
{
    attr_put(b, type, s, (u16)(strlen(s) + 1));
}

static void nest_begin(batch_t *b, u16 type)
{
    if (b->depth >= 8) { b->overflow = true; return; }
    b->nest[b->depth++] = b->len;
    nlattr_t *a = reserve(b, sizeof *a);
    a->nla_type = (u16)(type | NLA_F_NESTED);
    a->nla_len  = 0;                     /* nest_end fills this in */
}

static void nest_end(batch_t *b)
{
    if (b->depth <= 0 || b->overflow) return;
    u32 off = b->nest[--b->depth];
    nlattr_t *a = (nlattr_t *)(b->buf + off);
    a->nla_len = (u16)(b->len - off);
}

static void expr_begin(batch_t *b, const char *name)
{
    nest_begin(b, NFTA_LIST_ELEM);
    attr_str(b, NFTA_EXPR_NAME, name);
    nest_begin(b, NFTA_EXPR_DATA);
}

static void expr_end(batch_t *b)
{
    nest_end(b);        /* NFTA_EXPR_DATA */
    nest_end(b);        /* NFTA_LIST_ELEM */
}

static void e_cmp(batch_t *b, const void *data, u16 len)
{
    expr_begin(b, "cmp");
    attr_u32(b, NFTA_CMP_SREG, NFT_REG_1);
    attr_u32(b, NFTA_CMP_OP,   NFT_CMP_EQ);
    nest_begin(b, NFTA_CMP_DATA);
    attr_put(b, NFTA_DATA_VALUE, data, len);
    nest_end(b);
    expr_end(b);
}

static void e_verdict(batch_t *b, int verdict)
{
    expr_begin(b, "immediate");
    attr_u32(b, NFTA_IMMEDIATE_DREG, NFT_REG_VERDICT);
    nest_begin(b, NFTA_IMMEDIATE_DATA);
    nest_begin(b, NFTA_DATA_VERDICT);
    attr_u32(b, NFTA_VERDICT_CODE, (u32)verdict);
    nest_end(b);
    nest_end(b);
    expr_end(b);
}

/* "this packet is IPv4 and its source address is X -> drop".
 *
 * The nfproto test is not decoration. The table is family inet, so this
 * chain sees IPv6 packets too, and without it the rule would read four
 * bytes at offset 12 of an IPv6 header - which is the middle of the
 * source address - and drop whatever happened to match. */
static void rule_ban(batch_t *b, u32 addr_be, const char *label)
{
    u8     udata[64];
    size_t n = strlen(label) + 1;
    if (n > sizeof udata - 2) n = sizeof udata - 2;
    udata[0] = 0;                    /* NFTNL_UDATA_RULE_COMMENT */
    udata[1] = (u8)n;
    memcpy(udata + 2, label, n - 1);
    udata[2 + n - 1] = 0;

    msg_begin(b, NFT(NFT_MSG_NEWRULE),
              NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_APPEND);
    attr_str(b, NFTA_RULE_TABLE, TABLE_NAME);
    attr_str(b, NFTA_RULE_CHAIN, CHAIN_NAME);
    attr_put(b, NFTA_RULE_USERDATA, udata, (u16)(n + 2));
    nest_begin(b, NFTA_RULE_EXPRESSIONS);

    expr_begin(b, "meta");
    attr_u32(b, NFTA_META_KEY,  NFT_META_NFPROTO);
    attr_u32(b, NFTA_META_DREG, NFT_REG_1);
    expr_end(b);
    u8 fam = NFPROTO_IPV4;
    e_cmp(b, &fam, 1);

    expr_begin(b, "payload");
    attr_u32(b, NFTA_PAYLOAD_DREG,   NFT_REG_1);
    attr_u32(b, NFTA_PAYLOAD_BASE,   NFT_PAYLOAD_NETWORK_HEADER);
    attr_u32(b, NFTA_PAYLOAD_OFFSET, 12);      /* the IPv4 source */
    attr_u32(b, NFTA_PAYLOAD_LEN,    4);
    expr_end(b);
    e_cmp(b, &addr_be, 4);

    e_verdict(b, NF_DROP);

    nest_end(b);
    msg_end(b);
}

static int nl_open(void)
{
    long fd = lp_socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (fd < 0)
        return -1;
    sockaddr_nl_t me = { .nl_family = AF_NETLINK, .nl_pad = 0,
                         .nl_pid = 0, .nl_groups = 0 };
    if (lp_bind((int)fd, &me, sizeof me) < 0) {
        lp_close((int)fd);
        return -1;
    }
    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);
    return (int)fd;
}

static long nl_send(int fd, const void *buf, u32 len)
{
    sockaddr_nl_t kernel = { .nl_family = AF_NETLINK, .nl_pad = 0,
                             .nl_pid = 0, .nl_groups = 0 };
    return lp_sendto(fd, buf, len, 0, &kernel, sizeof kernel);
}

/* Read replies until the acknowledgement for last_seq. 0 if the whole
 * batch was accepted. quiet is for the "is the table there" probe,
 * where -ENOENT is the answer and not a fault. */
static int nl_wait(int fd, u32 last_seq, bool quiet)
{
    u8  buf[8192];
    int failed = 0;

    for (;;) {
        long n = lp_recvfrom(fd, buf, sizeof buf, 0, 0, 0);
        if (n <= 0) {
            if (!quiet && !failed)
                dprintf(STDERR_FILENO, "defend: the kernel did not answer\n");
            return -1;
        }
        u32 off = 0;
        while (off + sizeof(nlmsghdr_t) <= (u32)n) {
            nlmsghdr_t *h = (nlmsghdr_t *)(buf + off);
            if (h->nlmsg_len < sizeof(nlmsghdr_t) ||
                off + h->nlmsg_len > (u32)n) return failed ? -1 : 0;

            if (h->nlmsg_type == NLMSG_ERROR) {
                int err = *(int *)(buf + off + sizeof(nlmsghdr_t));
                if (err != 0) {
                    failed = 1;
                    if (!quiet)
                        dprintf(STDERR_FILENO,
                                "defend: the kernel refused a ban rule (%d)"
                                " - is this kernel built with nftables?\n",
                                -err);
                }
            }
            if (h->nlmsg_seq >= last_seq &&
                (h->nlmsg_type == NLMSG_ERROR || h->nlmsg_type == NLMSG_DONE))
                return failed ? -1 : 0;
            off += align4(h->nlmsg_len);
        }
    }
}

/* Asked outside a batch, so a missing table is an ordinary -ENOENT and
 * not something that aborts a transaction. */
static bool table_exists(int fd)
{
    batch_t b;
    memset(&b, 0, sizeof b);
    msg_begin(&b, NFT(NFT_MSG_GETTABLE), NLM_F_REQUEST | NLM_F_ACK);
    attr_str(&b, NFTA_TABLE_NAME, TABLE_NAME);
    msg_end(&b);
    if (nl_send(fd, b.buf, b.len) < 0) return false;
    return nl_wait(fd, b.seq, true) == 0;
}

/* ── the ban list ────────────────────────────────────────────────── */

typedef struct {
    u32 addr;     /* network order */
    s64 when;     /* unix seconds  */
    int fails;
} ban_t;

static ban_t bans[MAX_BANS];
static int   nbans;

static void load_bans(void)
{
    nbans = 0;
    long fd = lp_open(F_BANS, O_RDONLY, 0);
    if (fd < 0)
        return;

    s64  now = lp_time();
    char line[160];
    while (readline((int)fd, line, sizeof line) >= 0 && nbans < MAX_BANS) {
        if (line[0] == '#' || !line[0])
            continue;
        char *f[4];
        int   n = split(line, f, 4);
        if (n < 2)
            continue;
        u32 be;
        if (!ipv4_parse(f[0], &be))
            continue;
        s64 when = strtol(f[1], NULL, 10);
        /* Expired bans are simply not loaded, so the next save drops
         * them. A permanent list would eventually hold a home address
         * that has since been handed to somebody else, and an address
         * that is still attacking earns itself a new ban within ten
         * minutes of trying again. */
        if (now > 0 && when > 0 && now - when > BAN_SECS)
            continue;
        bans[nbans].addr  = be;
        bans[nbans].when  = when;
        bans[nbans].fails = (n > 2) ? atoi(f[2]) : 0;
        nbans++;
    }
    lp_close((int)fd);
}

static bool save_bans(void)
{
    /* Written aside and renamed over the top: a power cut halfway
     * through a rewrite would otherwise leave a truncated list, and a
     * truncated ban list is a machine that thinks it is blocking
     * somebody it is not. */
    long fd = lp_open(F_BANS ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    dprintf((int)fd,
            "# addresses defend has blocked, with when and how many\n"
            "# failures earned it. `defend unban <addr>` removes one.\n");
    for (int i = 0; i < nbans; i++) {
        char host[20];
        ipv4_format(bans[i].addr, host);
        dprintf((int)fd, "%s %ld %d\n", host, bans[i].when, bans[i].fails);
    }
    lp_close((int)fd);
    if (lp_rename(F_BANS ".new", F_BANS) != 0) {
        lp_unlink(F_BANS ".new");
        return false;
    }
    return true;
}

static bool is_banned(u32 be)
{
    for (int i = 0; i < nbans; i++)
        if (bans[i].addr == be) return true;
    return false;
}

/* Put the whole list in the kernel as one transaction. The table is
 * replaced wholesale rather than added to, which is what makes running
 * this twice give one ruleset and not two. */
static bool apply_bans(void)
{
    int fd = nl_open();
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "defend: no netfilter netlink socket - nothing can be"
                " blocked on this kernel\n");
        return false;
    }

    bool replacing = table_exists(fd);

    if (nbans == 0 && !replacing) { lp_close(fd); return true; }

    batch_t b;
    memset(&b, 0, sizeof b);
    msg_begin(&b, NFNL_MSG_BATCH_BEGIN, NLM_F_REQUEST);
    msg_end(&b);

    if (replacing) {
        msg_begin(&b, NFT(NFT_MSG_DELTABLE), NLM_F_REQUEST | NLM_F_ACK);
        attr_str(&b, NFTA_TABLE_NAME, TABLE_NAME);
        msg_end(&b);
    }

    if (nbans > 0) {
        msg_begin(&b, NFT(NFT_MSG_NEWTABLE),
                  NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE);
        attr_str(&b, NFTA_TABLE_NAME, TABLE_NAME);
        msg_end(&b);

        msg_begin(&b, NFT(NFT_MSG_NEWCHAIN),
                  NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE);
        attr_str(&b, NFTA_CHAIN_TABLE, TABLE_NAME);
        attr_str(&b, NFTA_CHAIN_NAME,  CHAIN_NAME);
        nest_begin(&b, NFTA_CHAIN_HOOK);
        attr_u32(&b, NFTA_HOOK_HOOKNUM,  NF_INET_LOCAL_IN);
        attr_u32(&b, NFTA_HOOK_PRIORITY, BAN_PRIORITY);
        nest_end(&b);
        /* accept, so everything that is not banned carries on to the
         * firewall's own chain. This chain decides nothing else. */
        attr_u32(&b, NFTA_CHAIN_POLICY, NF_ACCEPT);
        attr_str(&b, NFTA_CHAIN_TYPE,   "filter");
        msg_end(&b);

        for (int i = 0; i < nbans; i++) {
            char host[20], label[48];
            ipv4_format(bans[i].addr, host);
            snprintf(label, sizeof label, "banned: %s", host);
            rule_ban(&b, bans[i].addr, label);
        }
    }

    u32 ack_seq = b.seq;
    msg_begin(&b, NFNL_MSG_BATCH_END, NLM_F_REQUEST);
    msg_end(&b);

    if (b.overflow) {
        dprintf(STDERR_FILENO,
                "defend: too many bans to send in one transaction -"
                " trim %s\n", F_BANS);
        lp_close(fd);
        return false;
    }
    if (nl_send(fd, b.buf, b.len) < 0) {
        dprintf(STDERR_FILENO, "defend: could not reach the kernel\n");
        lp_close(fd);
        return false;
    }
    int rc = nl_wait(fd, ack_seq, false);
    lp_close(fd);
    if (rc != 0) {
        dprintf(STDERR_FILENO,
                "defend: nothing was blocked - the kernel takes the whole"
                " list or none of it\n");
        return false;
    }
    return true;
}

/* ── who is logged in over SSH right now ─────────────────────────────
 *
 * Needed before banning anybody. On a board with no keyboard and no
 * Ethernet, the address you are typing from may be the only route in
 * that exists; a tool that can lock its owner out of the machine it is
 * protecting gets turned off, and then it protects nothing. */

#define MAX_PEERS 16
static u32 peers[MAX_PEERS];
static int npeers;

static void scan_ssh_peers_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[512];
    int  lineno = 0;
    while (readline((int)fd, line, sizeof line) >= 0) {
        if (++lineno == 1)
            continue;                         /* the column header */
        char *f[12];
        if (split(line, f, 12) < 4)
            continue;
        if (hexn(f[3], 2) != 1)               /* 1 = ESTABLISHED */
            continue;
        u32 la, ra; u16 lp, rp;
        if (!hex_v4(f[1], &la, &lp) || lp != 22)
            continue;
        if (!hex_v4(f[2], &ra, &rp))
            continue;
        bool seen = false;
        for (int i = 0; i < npeers; i++)
            if (peers[i] == ra) seen = true;
        if (!seen && npeers < MAX_PEERS)
            peers[npeers++] = ra;
    }
    lp_close((int)fd);
}

static void find_ssh_peers(void)
{
    npeers = 0;
    scan_ssh_peers_file("/proc/net/tcp");
    scan_ssh_peers_file("/proc/net/tcp6");
}

static bool is_ssh_peer(u32 be)
{
    for (int i = 0; i < npeers; i++)
        if (peers[i] == be) return true;
    return false;
}

/* ── 1. SSH brute force ──────────────────────────────────────────────
 *
 * The source is /data/log/auth, which logd fills. Its format is worth
 * being exact about rather than guessing, because a parser that quietly
 * matches nothing is a brute-force detector that reports all clear
 * forever. logd writes every line as
 *
 *     2026-09-05 11:22:33 kernel: <what dropbear said>
 *
 * and it copies a line there only when the text matches one of its own
 * marks - "Bad password", "Login attempt", "Exit before auth" and the
 * rest, in logd.c's is_auth_line. So this file is already only about
 * people arriving, and the job here is to tell a failure from a success
 * and to pull the address out.
 *
 * dropbear's own wording, which is what those lines contain:
 *
 *   Bad password attempt for 'root' from 10.0.0.5:52134
 *   Login attempt for nonexistent user from 10.0.0.5:52134
 *   Exit before auth from <10.0.0.5:52134>: ...
 *   Password auth succeeded for 'root' from 10.0.0.5:52134
 *
 * The window is measured from the timestamp in the line rather than
 * from a saved file offset. An offset has to be right across restarts,
 * across log rotation and across a partition that was not mounted; a
 * timestamp is right by construction, and re-reading a 128KB file every
 * five minutes costs nothing on any board this runs on. */

typedef struct { u32 addr; int fails; s64 last; } source_t;
static source_t sources[MAX_SOURCES];
static int      nsources;

static int  auth_ok, auth_fail, auth_nonv4;

static int num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static bool line_time(const char *line, s64 *out)
{
    if (strlen(line) < 19) return false;
    if (line[4] != '-' || line[7] != '-' || line[10] != ' ' ||
        line[13] != ':' || line[16] != ':') return false;

    lp_tm_t tm;
    memset(&tm, 0, sizeof tm);
    tm.year = num(line, 4);
    tm.mon  = num(line + 5, 2);
    tm.day  = num(line + 8, 2);
    tm.hour = num(line + 11, 2);
    tm.min  = num(line + 14, 2);
    tm.sec  = num(line + 17, 2);
    if (tm.year < 0 || tm.mon < 0 || tm.day < 0 ||
        tm.hour < 0 || tm.min < 0 || tm.sec < 0) return false;
    *out = lp_timegm(&tm);
    return true;
}

/* The address after " from ". dropbear writes it bare on some lines and
 * inside angle brackets on others, and always with the client's port
 * after a colon. Returns false for anything that is not IPv4, and the
 * caller counts those separately rather than pretending. */
static bool line_addr(const char *line, u32 *be)
{
    const char *p = strstr(line, " from ");
    if (!p) return false;
    p += 6;
    if (*p == '<') p++;

    char buf[48];
    int  n = 0;
    while (*p && *p != ':' && *p != '>' && *p != ' ' &&
           n < (int)sizeof buf - 1)
        buf[n++] = *p++;
    buf[n] = '\0';
    return ipv4_parse(buf, be);
}

static bool is_failure(const char *line)
{
    static const char *marks[] = {
        "Bad password",
        "Login attempt for nonexistent user",
        "Exit before auth",
        "exit before auth",
        "bad signature",
        "bad packet length",
        NULL
    };
    if (strstr(line, "auth succeeded"))
        return false;
    for (int i = 0; marks[i]; i++)
        if (strstr(line, marks[i])) return true;
    return false;
}

static void count_source(u32 be, s64 when)
{
    for (int i = 0; i < nsources; i++)
        if (sources[i].addr == be) {
            sources[i].fails++;
            if (when > sources[i].last) sources[i].last = when;
            return;
        }
    if (nsources >= MAX_SOURCES)
        return;
    sources[nsources].addr  = be;
    sources[nsources].fails = 1;
    sources[nsources].last  = when;
    nsources++;
}

/* Returns true when the ban list changed. */
static bool check_auth(void)
{
    nsources = 0;
    auth_ok = auth_fail = auth_nonv4 = 0;

    long fd = lp_open(AUTH_LOG, O_RDONLY, 0);
    if (fd < 0) {
        printf("defend: no %s yet - nothing has tried to log in, or logd"
               " is not running (`service status logd`)\n", AUTH_LOG);
        return false;
    }

    s64 now = lp_time();
    s64 cut = now - WINDOW_SECS;

    char line[512];
    while (readline((int)fd, line, sizeof line) >= 0) {
        s64 when;
        if (!line_time(line, &when))
            continue;
        if (when < cut)
            continue;
        bool fail = is_failure(line);
        if (!fail && !strstr(line, "auth succeeded"))
            continue;                       /* a connection, not a verdict */

        u32 be;
        if (!line_addr(line, &be)) {
            if (fail) auth_nonv4++;
            continue;
        }
        if (fail) { auth_fail++; count_source(be, when); }
        else      { auth_ok++; }
    }
    lp_close((int)fd);

    printf("defend: ssh in the last %d minutes: %d failed, %d succeeded,"
           " from %d address%s\n",
           WINDOW_SECS / 60, auth_fail, auth_ok, nsources,
           nsources == 1 ? "" : "es");

    if (auth_nonv4 > 0)
        printf("defend:    %d failure%s came from an address that is not"
               " IPv4 - counted, not bannable (see -h)\n",
               auth_nonv4, auth_nonv4 == 1 ? "" : "s");

    find_ssh_peers();

    bool changed = false;
    for (int i = 0; i < nsources; i++) {
        if (sources[i].fails < FAIL_LIMIT)
            continue;

        char host[20], msg[192];
        ipv4_format(sources[i].addr, host);

        if (is_banned(sources[i].addr))
            continue;                       /* already dropped, say nothing */

        snprintf(msg, sizeof msg,
                 "ssh brute force from %s - %d failed logins in %d minutes",
                 host, sources[i].fails, WINDOW_SECS / 60);
        report(msg);

        /* Never take away the only way in.
         *
         * If this address holds the only established session on port 22,
         * it is very probably the person reading this, sitting behind
         * the same NAT as whatever is guessing passwords. Dropping it
         * ends that session and there is no console on this board. */
        if (is_ssh_peer(sources[i].addr) && npeers == 1) {
            printf("defend:    not banned - %s holds the only ssh session"
                   " open right now, and this board has no other way in\n",
                   host);
            printf("defend:    if that session is not yours: log in from"
                   " somewhere else first, then run `defend` again\n");
            continue;
        }
        if (sources[i].addr == htonl(0x7F000001u)) {
            printf("defend:    not banned - that is this machine talking"
                   " to itself\n");
            continue;
        }
        if (opt_dry) {
            printf("defend:    -n given, so nothing was blocked -"
                   " run `defend` without -n to block %s\n", host);
            continue;
        }
        if (nbans >= MAX_BANS) {
            printf("defend:    not banned - the list is full at %d."
                   " `defend status` shows it; `defend unban <addr>`"
                   " makes room\n", MAX_BANS);
            continue;
        }

        bans[nbans].addr  = sources[i].addr;
        bans[nbans].when  = now;
        bans[nbans].fails = sources[i].fails;
        nbans++;
        changed = true;
        printf("defend:    blocked. `defend unban %s` lets it back in\n",
               host);
    }
    return changed;
}

/* ── 2. files under /data that should not be there ───────────────────
 *
 * /data is mounted nosuid,nodev by /etc/rc, so a setuid file there
 * cannot actually gain anybody anything - which is exactly why finding
 * one matters: nothing legitimate on this system creates one, so it is
 * either somebody testing what the mount options are, or the leftovers
 * of an exploit that has not worked out yet.
 *
 * /data/debian and /data/python are skipped by default. A Debian tree
 * and a CPython install between them contain thousands of executables
 * and a handful of genuinely setuid binaries, and a report with three
 * thousand lines in it is a report nobody reads. -a includes them. */

static const char *SKIP_UNLESS_ALL[] = {
    "/data/debian", "/data/python", NULL
};

/* Where a program is meant to live. An executable bit anywhere else
 * under /data is worth a line: /data/log and /data/root hold data. */
static const char *PROGRAM_DIRS[] = {
    "/data/bin", "/data/pkg", "/data/python", "/data/debian",
    "/data/glibc", "/data/persist", "/data/sdk", NULL
};

#define MAX_LINES  30          /* per run, before we start counting only */
#define MAX_DEPTH  12

static int  fs_files, fs_dirs, fs_extra;
static bool fs_capped;

static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* /etc/passwd is read from disk on every lookup - there is no name
 * service here - so the answers are remembered. Without this, a walk of
 * a few thousand files reads the same file a few thousand times. */
#define UID_CACHE 32
static uid_t known_uid[UID_CACHE];
static bool  known_ok[UID_CACHE];
static int   nknown;

static bool uid_exists(uid_t uid)
{
    for (int i = 0; i < nknown; i++)
        if (known_uid[i] == uid) return known_ok[i];

    lp_user_t u;
    bool ok = lp_user_by_uid(uid, &u);
    if (nknown < UID_CACHE) {
        known_uid[nknown] = uid;
        known_ok[nknown]  = ok;
        nknown++;
    }
    return ok;
}

static void fs_note(const char *headline, const char *fix)
{
    if (fs_extra + findings >= MAX_LINES) { fs_extra++; return; }
    report(headline);
    if (fix && *fix)
        printf("defend:    %s\n", fix);
}

static void walk(const char *path, int depth)
{
    if (depth > MAX_DEPTH) {
        fs_capped = true;
        return;
    }

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[2048];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char full[1024];
            if (snprintf(full, sizeof full, "%s/%s", path, name)
                    >= (int)sizeof full)
                continue;

            if (!opt_all) {
                bool skip = false;
                for (int i = 0; SKIP_UNLESS_ALL[i]; i++)
                    if (strcmp(full, SKIP_UNLESS_ALL[i]) == 0) skip = true;
                if (skip)
                    continue;
            }

            lp_stat_t st;
            if (lp_stat(full, &st, false) != 0)
                continue;

            u32 type = st.mode & LP_S_IFMT;
            u32 perm = st.mode & 07777;

            /* A symlink's own mode bits mean nothing on Linux - they
             * are always 0777 - and following it would walk out of
             * /data entirely. Neither is worth a line. */
            if (type == LP_S_IFLNK)
                continue;

            if (!uid_exists(st.uid)) {
                char h[256], f[192];
                snprintf(h, sizeof h,
                         "%s is owned by uid %u, and there is no such user",
                         full, (u32)st.uid);
                snprintf(f, sizeof f,
                         "a file left by a user that was deleted, or"
                         " unpacked from somebody else's archive -"
                         " `chown root %s` or delete it", full);
                fs_note(h, f);
            }

            if (type == LP_S_IFDIR) {
                fs_dirs++;
                if ((perm & 0002) && !(perm & S_ISVTX)) {
                    char h[256], f[192];
                    snprintf(h, sizeof h,
                             "%s is world-writable and has no sticky bit",
                             full);
                    snprintf(f, sizeof f,
                             "anybody can delete or replace another user's"
                             " files in it - `chmod 1777 %s`, or 0755 if it"
                             " was never meant to be shared", full);
                    fs_note(h, f);
                }
                walk(full, depth + 1);
                continue;
            }

            if (type != LP_S_IFREG)
                continue;
            fs_files++;

            if (perm & (S_ISUID | S_ISGID)) {
                char h[256], f[192];
                snprintf(h, sizeof h, "%s is set%s (mode %04o)", full,
                         (perm & S_ISUID) ? "uid" : "gid", perm);
                snprintf(f, sizeof f,
                         "/data is mounted nosuid, so this gains nothing"
                         " and nothing here creates one - `chmod %04o %s`"
                         " or delete it",
                         perm & ~(u32)(S_ISUID | S_ISGID), full);
                fs_note(h, f);
            }

            if (perm & 0002) {
                char h[256], f[192];
                snprintf(h, sizeof h, "%s is world-writable (mode %04o)",
                         full, perm);
                snprintf(f, sizeof f, "anybody who gets a shell can rewrite"
                         " it - `chmod %04o %s`", perm & ~(u32)0002, full);
                fs_note(h, f);
            }

            if (perm & 0111) {
                bool allowed = false;
                for (int i = 0; PROGRAM_DIRS[i]; i++)
                    if (under(full, PROGRAM_DIRS[i])) allowed = true;
                /* integrity already watches rc.local, and it has to be
                 * runnable to do its job. */
                if (strcmp(full, "/data/rc.local") == 0)
                    allowed = true;
                if (!allowed) {
                    char h[256], f[192];
                    snprintf(h, sizeof h,
                             "%s is executable (mode %04o), and that is a"
                             " place for data", full, perm);
                    snprintf(f, sizeof f,
                             "programs belong in /data/bin - `chmod %04o %s`"
                             " if it is not one", perm & ~(u32)0111, full);
                    fs_note(h, f);
                }
            }
        }
    }
    lp_close((int)fd);
}

static void check_files(void)
{
    fs_files = fs_dirs = fs_extra = 0;
    fs_capped = false;
    nknown = 0;

    if (!lp_is_dir("/data")) {
        printf("defend: /data is not mounted - nothing to walk\n");
        return;
    }

    int before = findings;
    walk("/data", 0);

    printf("defend: /data: %d files in %d directories, %d worth a look%s\n",
           fs_files, fs_dirs, findings - before,
           opt_all ? "" : " (/data/debian and /data/python skipped, -a"
                          " includes them)");
    if (fs_extra > 0)
        printf("defend:    and %d more of the same, not listed - deal with"
               " the ones above and run it again\n", fs_extra);
    if (fs_capped)
        printf("defend:    stopped at %d directories deep; anything below"
               " that was not looked at\n", MAX_DEPTH);
}

/* ── 3. what is listening ────────────────────────────────────────────
 *
 * A new listening socket on a board whose software has not changed is
 * the single most useful signal there is: nothing on this system starts
 * listening on its own, so either somebody installed something, or
 * something installed itself.
 *
 * Which process holds a socket is not in /proc/net at all. The only
 * link is the socket's inode, which turns up again as the target of a
 * /proc/<pid>/fd/<n> symlink reading "socket:[NNN]" - so the map costs
 * a readlink per open descriptor of every process. It is built once for
 * all four socket files. */

#define OWNER_MAX 256
typedef struct { u64 ino; int pid; char name[16]; } owner_t;
static owner_t owners[OWNER_MAX];
static int     nowners;

static bool numeric(const char *s)
{
    if (!*s) return false;
    for (const char *c = s; *c; c++)
        if (*c < '0' || *c > '9') return false;
    return true;
}

static void scan_fds(int pid, const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/fd", pid);

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[2048];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec = buf + off;
            u16         len = *(u16 *)(rec + DIRENT_RECLEN);
            const char *ent = rec + DIRENT_NAME;
            off += len;
            if (!numeric(ent))
                continue;

            char link[80], target[64];
            snprintf(link, sizeof link, "/proc/%d/fd/%s", pid, ent);
            long r = lp_readlink(link, target, sizeof target - 1);
            if (r <= 0)
                continue;
            target[r] = '\0';
            if (strncmp(target, "socket:[", 8) != 0)
                continue;
            if (nowners >= OWNER_MAX)
                continue;
            owners[nowners].ino = (u64)strtol(target + 8, NULL, 10);
            owners[nowners].pid = pid;
            strlcpy(owners[nowners].name, name, sizeof owners[0].name);
            nowners++;
        }
    }
    lp_close((int)fd);
}

static void build_owners(void)
{
    nowners = 0;
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec = buf + off;
            u16         len = *(u16 *)(rec + DIRENT_RECLEN);
            const char *ent = rec + DIRENT_NAME;
            off += len;
            if (!numeric(ent))
                continue;

            int  pid = (int)strtol(ent, NULL, 10);
            char path[64], name[64];
            snprintf(path, sizeof path, "/proc/%d/comm", pid);
            if (proc_read(path, name, sizeof name) <= 0)
                strlcpy(name, "?", sizeof name);
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            scan_fds(pid, name);
        }
    }
    lp_close((int)fd);
}

static void owner_of(u64 ino, char *out, size_t cap)
{
    for (int i = 0; i < nowners; i++)
        if (owners[i].ino == ino) {
            snprintf(out, cap, "%d/%s", owners[i].pid, owners[i].name);
            return;
        }
    strlcpy(out, "-", cap);
}

/* The baseline, held as text lines exactly as they are written to the
 * file: "listen tcp 0.0.0.0:22 dropbear" and "key SHA256:... comment".
 * Comparing text keeps the file readable and editable, which matters
 * for something an owner is asked to trust. */
#define MAX_BASE 96
static char base_line[MAX_BASE][160];
static int  nbase;
static s64  base_when;

static void load_baseline(void)
{
    nbase = 0;
    base_when = 0;

    long fd = lp_open(F_BASELINE, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[256];
    while (readline((int)fd, line, sizeof line) >= 0 && nbase < MAX_BASE) {
        if (strncmp(line, "# recorded ", 11) == 0)
            base_when = strtol(line + 11, NULL, 10);
        if (line[0] == '#' || !line[0])
            continue;
        strlcpy(base_line[nbase++], line, sizeof base_line[0]);
    }
    lp_close((int)fd);
}

/* Does the baseline hold a line starting with this prefix? Used both
 * for the whole "listen tcp 1.2.3.4:80" key and for a key fingerprint. */
static const char *base_find(const char *prefix)
{
    size_t n = strlen(prefix);
    for (int i = 0; i < nbase; i++)
        if (strncmp(base_line[i], prefix, n) == 0 &&
            (base_line[i][n] == ' ' || base_line[i][n] == '\0'))
            return base_line[i];
    return NULL;
}

/* Collected here so `defend baseline` can write the same lines that
 * `defend` compares against - one function producing both is the only
 * way they cannot drift apart. */
#define MAX_LISTEN 64
static char listen_line[MAX_LISTEN][160];
static int  nlisten;

static void collect_file(const char *path, const char *proto, bool udp)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;                    /* no IPv6 in this kernel, say */

    char line[512];
    int  lineno = 0;
    while (readline((int)fd, line, sizeof line) >= 0) {
        if (++lineno == 1)
            continue;
        char *f[12];
        if (split(line, f, 12) < 10)
            continue;

        u32 st = hexn(f[3], 2);
        /* TCP 10 is LISTEN. A UDP socket never listens; the kernel
         * writes 1 once connect() has pinned a peer to it, so anything
         * else is a socket sitting open for whatever arrives - which is
         * the same exposure and belongs in the same list. */
        bool listening = udp ? (st != 1) : (st == 10);
        if (!listening)
            continue;

        char addr[64], who[32];
        fmt_sock(f[1], addr, sizeof addr);
        owner_of((u64)strtol(f[9], NULL, 10), who, sizeof who);

        if (nlisten >= MAX_LISTEN)
            return;
        snprintf(listen_line[nlisten++], sizeof listen_line[0],
                 "listen %s %s %s", proto, addr, who);
    }
    lp_close((int)fd);
}

static void collect_listeners(void)
{
    nlisten = 0;
    build_owners();
    collect_file("/proc/net/tcp",  "tcp",  false);
    collect_file("/proc/net/tcp6", "tcp6", false);
    collect_file("/proc/net/udp",  "udp",  true);
    collect_file("/proc/net/udp6", "udp6", true);
}

static void check_listeners(void)
{
    collect_listeners();

    printf("defend: listening: %d socket%s\n",
           nlisten, nlisten == 1 ? "" : "s");

    if (nbase == 0) {
        for (int i = 0; i < nlisten; i++)
            printf("defend:    %s\n", listen_line[i]);
        printf("defend:    no baseline recorded - run `defend baseline`"
               " once this is the set you expect, and anything new after"
               " that gets reported\n");
        return;
    }

    for (int i = 0; i < nlisten; i++) {
        /* The key is "listen <proto> <addr:port>"; the holder is the
         * rest of the line and is compared separately, because the same
         * port held by a different program is its own kind of news. */
        char key[160];
        strlcpy(key, listen_line[i], sizeof key);
        char *sp = strchr(key, ' ');
        sp = sp ? strchr(sp + 1, ' ') : NULL;
        sp = sp ? strchr(sp + 1, ' ') : NULL;
        if (sp) *sp = '\0';

        const char *was = base_find(key);
        const char *who = strrchr(listen_line[i], ' ');
        who = who ? who + 1 : "-";

        if (!was) {
            char msg[224];
            snprintf(msg, sizeof msg,
                     "new listener: %s, held by %s - it was not there when"
                     " the baseline was recorded", key + 7, who);
            report(msg);
            printf("defend:    `netstat -lp` shows it, `ps` shows the"
                   " process. If it is yours: `defend baseline`\n");
            continue;
        }

        const char *waswho = strrchr(was, ' ');
        waswho = waswho ? waswho + 1 : "-";
        if (strcmp(waswho, "-") != 0 && strcmp(who, "-") != 0 &&
            strcmp(waswho, who) != 0) {
            char msg[224];
            snprintf(msg, sizeof msg,
                     "%s is now held by %s, and the baseline says %s",
                     key + 7, who, waswho);
            report(msg);
            printf("defend:    a different program answering on a port"
                   " that was already open. `ps` shows it\n");
        }
    }
}

/* ── 4. integrity ────────────────────────────────────────────────────
 *
 * integrity(1) already hashes everything on this machine that survives
 * a reboot, and its list is the whole value of it - a persistence path
 * that is not on that list is one nobody is watching. Writing a second
 * hasher here would mean two lists to keep in step, and the one that
 * was forgotten would be the one that mattered. So this runs it.
 *
 * -c so that it checks and records nothing: accepting a change is a
 * decision a person makes, with `integrity -u`, not something a
 * background service does on their behalf. Its own output is already
 * specific about which file changed and why that file matters, so it
 * goes straight to the terminal and this only adds the count. */
static void check_integrity(void)
{
    if (!lp_exists("/bin/integrity")) {
        report("integrity is not installed - nothing is checking the files"
               " that survive a reboot");
        printf("defend:    /data/rc.local, authorized_keys, /data/users"
               " are the ones that decide whether something runs again\n");
        return;
    }

    char *const argv[] = { (char *)"integrity", (char *)"-c", NULL };
    pid_t pid = lp_fork();
    if (pid < 0) {
        printf("defend: could not run integrity (out of processes?)\n");
        return;
    }
    if (pid == 0) {
        lp_execve("/bin/integrity", argv, environ);
        lp_exit(127);
    }

    int status = 0;
    lp_waitpid(pid, &status, 0);
    if (LP_WIFEXITED(status) && LP_WEXITSTATUS(status) == 1) {
        findings++;
        if (opt_daemon)
            lp_log("defend", "integrity: a file that survives a reboot"
                             " has changed");
        printf("defend:    the lines above are integrity's. `integrity -u`"
               " accepts the change if it was yours\n");
    }
}

/* ── 5. authorized_keys ──────────────────────────────────────────────
 *
 * Every key that can log in, with its comment and its fingerprint in
 * the form ssh-keygen prints, so the owner can hold it next to what
 * their laptop says and see whether it is theirs.
 *
 * All three files are read, not only the live one. authkey merges
 * /boot and the image into /root/.ssh/authorized_keys at boot, so a key
 * added to the boot partition is not live yet - but it will be at the
 * next reboot, and a key that appears there came from somebody holding
 * the card. That is worth saying now rather than after the reboot. */

static const struct { const char *path; const char *where; } KEYFILES[] = {
    { "/root/.ssh/authorized_keys", "live" },
    { "/boot/authorized_keys",      "boot partition" },
    { "/etc/authorized_keys",       "system image" },
    { NULL, NULL }
};

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *s, u8 *out, int cap)
{
    u32 acc = 0;
    int bits = 0, n = 0;
    for (; *s && *s != '='; s++) {
        int v = b64val(*s);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (u8)(acc >> bits);
        }
    }
    return n;
}

/* No padding, because that is how OpenSSH prints a SHA256 fingerprint. */
static void b64_encode(const u8 *in, int n, char *out)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16;
        int have = 1;
        if (i + 1 < n) { v |= (u32)in[i + 1] << 8; have++; }
        if (i + 2 < n) { v |= (u32)in[i + 2];      have++; }
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (have > 1) out[o++] = t[(v >> 6) & 63];
        if (have > 2) out[o++] = t[v & 63];
    }
    out[o] = '\0';
}

/* SHA-256 of the decoded key blob, base64, which is what
 * `ssh-keygen -lf` prints and what every client shows on first connect.
 *
 * The hash comes from lp_sha256_file, so there is one SHA-256 in this
 * system and not two - the same reason integrity writes a directory
 * listing to a temporary file and hashes that.
 *
 * O_EXCL after an unlink, because /tmp is 1777 and this runs as root: a
 * fixed name opened O_CREAT|O_TRUNC would let anybody pre-create it as
 * a symlink and have root truncate whatever it points at. */
static bool fingerprint(const char *line, char *out, size_t cap)
{
    while (*line == ' ' || *line == '\t') line++;
    const char *sp = strchr(line, ' ');
    if (!sp) return false;
    const char *body = sp + 1;
    while (*body == ' ') body++;
    const char *end = strchr(body, ' ');
    size_t blen = end ? (size_t)(end - body) : strlen(body);

    char b64[1024];
    if (blen == 0 || blen >= sizeof b64) return false;
    memcpy(b64, body, blen);
    b64[blen] = '\0';

    u8  blob[768];
    int n = b64_decode(b64, blob, (int)sizeof blob);
    if (n <= 0) return false;

    const char *tmp = "/tmp/.defend.key";
    lp_unlink(tmp);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    bool ok = lp_write((int)fd, blob, (size_t)n) == (long)n;
    lp_close((int)fd);

    char hex[72];
    ok = ok && lp_sha256_file(tmp, hex);
    lp_unlink(tmp);
    if (!ok || strlen(hex) < 64) return false;

    u8 digest[32];
    for (int i = 0; i < 32; i++)
        digest[i] = (u8)hexn(hex + i * 2, 2);
    char fp[48];
    b64_encode(digest, 32, fp);
    snprintf(out, cap, "SHA256:%s", fp);
    return true;
}

/* The trailing comment - usually who the key belongs to. */
static void key_comment(const char *line, char *out, size_t cap)
{
    while (*line == ' ' || *line == '\t') line++;
    const char *sp = strchr(line, ' ');
    const char *body = sp ? sp + 1 : NULL;
    while (body && *body == ' ') body++;
    const char *end = body ? strchr(body, ' ') : NULL;
    if (!end) { strlcpy(out, "(no comment)", cap); return; }
    while (*end == ' ') end++;
    if (!*end) { strlcpy(out, "(no comment)", cap); return; }
    strlcpy(out, end, cap);
}

static char key_line[MAX_BASE][160];
static int  nkeylines;

static void collect_keys(void)
{
    nkeylines = 0;
    for (int k = 0; KEYFILES[k].path; k++) {
        long fd = lp_open(KEYFILES[k].path, O_RDONLY, 0);
        if (fd < 0)
            continue;
        char line[1024];
        while (readline((int)fd, line, sizeof line) >= 0) {
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '#')
                continue;
            if (nkeylines >= MAX_BASE)
                break;

            char fp[64], comment[80];
            if (!fingerprint(line, fp, sizeof fp))
                strlcpy(fp, "SHA256:(unreadable)", sizeof fp);
            key_comment(line, comment, sizeof comment);
            snprintf(key_line[nkeylines++], sizeof key_line[0],
                     "key %s %s %s", fp, KEYFILES[k].where, comment);
        }
        lp_close((int)fd);
    }
}

static void check_keys(void)
{
    collect_keys();

    if (nkeylines == 0) {
        report("no SSH key is authorized anywhere - nobody can log in,"
               " including you");
        printf("defend:    put your public key in authorized_keys on the"
               " boot partition (it is FAT32) and reboot. `authkey -l`\n");
        return;
    }

    printf("defend: %d ssh key%s can log in:\n",
           nkeylines, nkeylines == 1 ? "" : "s");
    for (int i = 0; i < nkeylines; i++)
        printf("defend:    %s\n", key_line[i] + 4);

    if (nbase == 0) {
        printf("defend:    no baseline recorded - `defend baseline`, and a"
               " key added after that gets reported\n");
        return;
    }

    for (int i = 0; i < nkeylines; i++) {
        /* Matched on the fingerprint alone: the same key added again
         * under a new comment, or copied to another of the three files,
         * is still the same key and is not news. */
        char key[96];
        strlcpy(key, key_line[i], sizeof key);
        char *sp = strchr(key, ' ');
        sp = sp ? strchr(sp + 1, ' ') : NULL;
        if (sp) *sp = '\0';

        if (base_find(key))
            continue;

        char msg[224];
        snprintf(msg, sizeof msg,
                 "an ssh key that was not in the baseline can log in: %s",
                 key_line[i] + 4);
        report(msg);
        printf("defend:    if you did not add it, that is how somebody"
               " keeps getting back in - edit /root/.ssh/authorized_keys"
               " and `defend baseline`\n");
    }
}

/* ── the run ─────────────────────────────────────────────────────── */

static s64 st_checks, st_last, st_banned_total;

static void load_state(void)
{
    long fd = lp_open(F_STATE, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[128];
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp++ = '\0';
        s64 v = strtol(sp, NULL, 10);
        if      (strcmp(line, "checks") == 0)  st_checks = v;
        else if (strcmp(line, "last") == 0)    st_last = v;
        else if (strcmp(line, "banned") == 0)  st_banned_total = v;
    }
    lp_close((int)fd);
}

static void save_state(void)
{
    long fd = lp_open(F_STATE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    dprintf((int)fd, "checks %ld\nlast %ld\nbanned %ld\n",
            st_checks, st_last, st_banned_total);
    lp_close((int)fd);
}

static void when_str(s64 t, char *out, size_t cap)
{
    if (t <= 0) { strlcpy(out, "never", cap); return; }
    lp_tm_t tm;
    lp_gmtime(t, &tm);
    snprintf(out, cap, "%d-%02d-%02d %02d:%02d:%02d UTC",
             tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
}

static int run_once(bool full)
{
    findings = 0;
    load_baseline();

    int  before = nbans;
    bool changed = check_auth();
    check_listeners();
    if (full) {
        check_files();
        check_keys();
        check_integrity();
    }

    if (changed) {
        st_banned_total += nbans - before;
        if (!save_bans())
            dprintf(STDERR_FILENO,
                    "defend: cannot write %s - is /data mounted?\n", F_BANS);
        if (!apply_bans())
            dprintf(STDERR_FILENO,
                    "defend: the bans are recorded but not in force -"
                    " they will be tried again at the next check\n");
    } else if (nbans > 0) {
        /* The kernel keeps no table across a reboot, so the file is the
         * truth and this puts it back whenever it is missing. */
        int fd = nl_open();
        if (fd >= 0) {
            bool live = table_exists(fd);
            lp_close(fd);
            if (!live && apply_bans())
                printf("defend: put %d ban%s back in the kernel after a"
                       " restart\n", nbans, nbans == 1 ? "" : "s");
        }
    }

    st_checks++;
    st_last = lp_time();
    save_state();

    if (findings == 0)
        printf("defend: nothing to report\n");
    else
        printf("defend: %d thing%s above want%s a decision\n",
               findings, findings == 1 ? "" : "s", findings == 1 ? "s" : "");
    return findings ? 1 : 0;
}

/* ── the subcommands ─────────────────────────────────────────────── */

static int cmd_status(void)
{
    char buf[64];

    load_state();
    load_bans();
    load_baseline();

    when_str(st_last, buf, sizeof buf);
    printf("checks run     %ld, last at %s\n", st_checks, buf);

    when_str(base_when, buf, sizeof buf);
    printf("baseline       %s", buf);
    if (nbase == 0)
        printf("  - nothing recorded; run `defend baseline`");
    else
        printf("  (%d items)", nbase);
    printf("\n");

    printf("blocked        %d now, %ld since this file was created\n",
           nbans, st_banned_total);

    if (nbans > 0) {
        s64 now = lp_time();
        printf("\n%-16s %-8s %-24s %s\n",
               "address", "failures", "blocked at", "expires in");
        for (int i = 0; i < nbans; i++) {
            char host[20], at[48];
            ipv4_format(bans[i].addr, host);
            when_str(bans[i].when, at, sizeof at);
            s64 left = bans[i].when + BAN_SECS - now;
            if (left < 0) left = 0;
            printf("%-16s %-8d %-24s %ld hours\n",
                   host, bans[i].fails, at, left / 3600);
        }
        printf("\n`defend unban <address>` lets one back in.\n");
    }

    /* Is what is written down actually in force? A ban list that the
     * kernel has never been told about is the failure this command
     * exists to make visible. */
    int fd = nl_open();
    if (fd >= 0) {
        bool live = table_exists(fd);
        lp_close(fd);
        if (nbans > 0 && !live)
            printf("\n** the bans above are NOT in the kernel - run"
                   " `defend` to put them back\n");
    } else if (nbans > 0) {
        printf("\n** this kernel has no netfilter netlink socket, so"
               " nothing is actually being blocked\n");
    }
    return 0;
}

static int cmd_unban(const char *addr)
{
    u32 be;
    if (!ipv4_parse(addr, &be)) {
        dprintf(STDERR_FILENO,
                "defend: \"%s\" is not an IPv4 address -"
                " `defend status` lists what is blocked\n", addr);
        return 2;
    }

    load_bans();
    int keep = 0;
    bool had = false;
    for (int i = 0; i < nbans; i++) {
        if (bans[i].addr == be) { had = true; continue; }
        bans[keep++] = bans[i];
    }
    nbans = keep;

    if (!had) {
        printf("defend: %s was not blocked - `defend status` lists what"
               " is\n", addr);
        return 1;
    }
    if (!save_bans()) {
        dprintf(STDERR_FILENO,
                "defend: cannot write %s - is /data mounted?\n", F_BANS);
        return 1;
    }
    if (!apply_bans())
        return 1;
    printf("defend: %s can connect again\n", addr);
    printf("defend:   it earns a new ban after %d failed logins in %d"
           " minutes\n", FAIL_LIMIT, WINDOW_SECS / 60);
    return 0;
}

static int cmd_baseline(void)
{
    collect_listeners();
    collect_keys();

    long fd = lp_open(F_BASELINE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "defend: cannot write %s - is /data mounted?\n", F_BASELINE);
        return 1;
    }
    dprintf((int)fd,
            "# What defend expects to find. Anything not listed here is\n"
            "# reported. Edit it by hand or run `defend baseline` again.\n");
    dprintf((int)fd, "# recorded %ld\n", lp_time());
    for (int i = 0; i < nlisten; i++)
        dprintf((int)fd, "%s\n", listen_line[i]);
    for (int i = 0; i < nkeylines; i++)
        dprintf((int)fd, "%s\n", key_line[i]);
    lp_close((int)fd);

    if (lp_rename(F_BASELINE ".new", F_BASELINE) != 0) {
        lp_unlink(F_BASELINE ".new");
        dprintf(STDERR_FILENO, "defend: could not replace %s\n", F_BASELINE);
        return 1;
    }

    printf("defend: recorded %d listening socket%s and %d ssh key%s\n",
           nlisten, nlisten == 1 ? "" : "s",
           nkeylines, nkeylines == 1 ? "" : "s");
    for (int i = 0; i < nlisten; i++)
        printf("  %s\n", listen_line[i] + 7);
    for (int i = 0; i < nkeylines; i++)
        printf("  %s\n", key_line[i] + 4);
    printf("\nAnything that appears after this is reported by `defend`.\n");
    printf("Run it again after you install something that listens.\n");
    return 0;
}

static void usage(void)
{
    printf("usage: defend [-d] [-i seconds] [-a] [-n]\n");
    printf("       defend status | unban <address> | baseline\n\n");
    printf("This is not an antivirus: there is no signature database,\n");
    printf("because one would be tens of megabytes against a whole\n");
    printf("system of 13-22MB and useless the week it stopped being\n");
    printf("updated. It checks the five things that actually happen to\n");
    printf("a small internet-facing board instead.\n\n");
    printf("  1  ssh brute force in %s, blocked past %d failures\n",
           AUTH_LOG, FAIL_LIMIT);
    printf("     in %d minutes (IPv4 sources only - the drop rule it\n",
           WINDOW_SECS / 60);
    printf("     writes matches an IPv4 source address)\n");
    printf("  2  files under /data that should not be there: setuid,\n");
    printf("     world-writable, owned by no user, executable in a\n");
    printf("     place that holds data\n");
    printf("  3  every listening socket and who holds it, against the\n");
    printf("     baseline\n");
    printf("  4  integrity(1), for what survives a reboot\n");
    printf("  5  every ssh key that can log in, with its fingerprint\n\n");
    printf("  -d           keep running, checking every %d seconds\n",
           DEFAULT_INTERVAL);
    printf("  -i <seconds> change that interval\n");
    printf("  -a           walk /data/debian and /data/python too\n");
    printf("  -n           say what would be blocked, block nothing\n\n");
    printf("State is in %s. Exit 1 means something wants a decision.\n",
           DIR_STATE);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    const char *arg = NULL;

    /* One loop over argv, so a value consumed by an option here can
     * never be mistaken for a subcommand further down. That bug has
     * shipped in this tree before. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ||
            strcmp(a, "help") == 0) { usage(); return 0; }
        if (strcmp(a, "-d") == 0) { opt_daemon = true; continue; }
        if (strcmp(a, "-a") == 0) { opt_all = true; continue; }
        if (strcmp(a, "-n") == 0) { opt_dry = true; continue; }
        if (strcmp(a, "-i") == 0) {
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO,
                        "defend: -i wants a number of seconds,"
                        " e.g. `defend -d -i 60`\n");
                return 2;
            }
            interval = atoi(argv[++i]);
            if (interval < 10) {
                dprintf(STDERR_FILENO,
                        "defend: -i %s is too short; 10 seconds is the"
                        " floor, and %d is the sensible value\n",
                        argv[i], DEFAULT_INTERVAL);
                return 2;
            }
            continue;
        }
        if (a[0] == '-') {
            dprintf(STDERR_FILENO,
                    "defend: unknown option %s - `defend -h`\n", a);
            return 2;
        }
        if (!cmd)      cmd = a;
        else if (!arg) arg = a;
        else {
            dprintf(STDERR_FILENO,
                    "defend: too many arguments - `defend -h`\n");
            return 2;
        }
    }

    lp_mkdir(DIR_STATE, 0700);

    if (cmd) {
        if (strcmp(cmd, "status") == 0)
            return cmd_status();
        if (strcmp(cmd, "baseline") == 0)
            return cmd_baseline();
        if (strcmp(cmd, "unban") == 0) {
            if (!arg) {
                dprintf(STDERR_FILENO,
                        "defend: which address? `defend status` lists what"
                        " is blocked\n");
                return 2;
            }
            return cmd_unban(arg);
        }
        dprintf(STDERR_FILENO,
                "defend: no such command \"%s\" - status, unban, baseline,"
                " or nothing at all to run the checks\n", cmd);
        return 2;
    }

    load_state();
    load_bans();

    if (!opt_daemon)
        return run_once(true);

    /* ── as a service ──
     *
     * Every pass does the two cheap checks: the auth log is one file
     * and the socket tables are four, and both are the ones where being
     * late matters - a password guesser gets a few hundred more tries
     * for every minute this waits.
     *
     * The file walk, the key fingerprints and integrity go round once
     * an hour. Walking /data and hashing every key every five minutes
     * would be the largest thing this board does all day, for answers
     * that change about as often as somebody logs in. The first pass
     * after a start is a full one, so a reboot always gets a complete
     * check.
     *
     * No forking. init supervises a child that stays in the foreground;
     * a process that goes to the background looks like an instant death
     * and would be restarted forever. */
    printf("defend: watching. every %d seconds, and a full scan every"
           " %d minutes\n", interval, (interval * FULL_EVERY) / 60);

    for (s64 pass = 0; ; pass++) {
        run_once(pass % FULL_EVERY == 0);
        lp_sleep_ms((long)interval * 1000);
    }
}
