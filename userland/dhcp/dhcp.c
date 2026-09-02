/* dhcp - a minimal DHCP client (RFC 2131).
 *
 * Why write our own: DHCP is a plain UDP protocol with no cryptography.
 * Unlike SSH it is not dangerous to implement yourself, so this removes
 * one external dependency. Under 400 lines, smaller than udhcpc or dhcpcd.
 *
 * The exchange:
 *   DISCOVER (broadcast)  ->  OFFER  (the server proposes an address)
 *   REQUEST  (broadcast)  ->  ACK    (it is settled)
 * Once settled we set the address and netmask on the interface, add the
 * default route and write /etc/resolv.conf.
 *
 * Not handled: lease renewal, DECLINE, choosing among several servers.
 * None of that is needed to join one home router.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68
#define DHCP_MAGIC         0x63825363u

/* op */
#define BOOTREQUEST  1
#define BOOTREPLY    2

/* Option 53 values */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

/* Option numbers */
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_LIST   55
#define OPT_END         255

#define BOOTP_FIXED_LEN  236        /* fixed part, before the options */
#define PKT_SIZE         576        /* the RFC's minimum DHCP message size */

/* BOOTP/DHCP header. This is a wire layout, so it must be packed. */
typedef struct __attribute__((packed)) {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic;
    u8  options[PKT_SIZE - BOOTP_FIXED_LEN - 4];
} dhcp_pkt_t;

typedef struct {
    u32 addr;       /* the address we got (network order) */
    u32 mask;
    u32 router;
    u32 dns;
    u32 server;
    u32 lease;      /* seconds (host order) */
} lease_t;

/* ── Building options ── */

static u8 *put_opt(u8 *p, u8 code, u8 len, const void *val)
{
    *p++ = code;
    *p++ = len;
    if (len) { memcpy(p, val, len); p += len; }
    return p;
}

/* ── Parsing options ──
 * Trusting the length field would let a crafted packet read past the
 * buffer. Always check it against the end. */
static bool parse_options(const u8 *opt, size_t len, lease_t *out, u8 *msg_type)
{
    const u8 *end = opt + len;
    *msg_type = 0;

    while (opt < end) {
        u8 code = *opt++;

        if (code == 0) continue;            /* padding */
        if (code == OPT_END) break;
        if (opt >= end) return false;       /* no length byte */

        u8 olen = *opt++;
        if (opt + olen > end) return false; /* the value runs past the buffer */

        switch (code) {
        case OPT_MSG_TYPE:
            if (olen >= 1) *msg_type = opt[0];
            break;
        case OPT_SUBNET_MASK:
            if (olen >= 4) memcpy(&out->mask, opt, 4);
            break;
        case OPT_ROUTER:
            if (olen >= 4) memcpy(&out->router, opt, 4);
            break;
        case OPT_DNS:
            if (olen >= 4) memcpy(&out->dns, opt, 4);
            break;
        case OPT_SERVER_ID:
            if (olen >= 4) memcpy(&out->server, opt, 4);
            break;
        case OPT_LEASE_TIME:
            if (olen >= 4) {
                u32 v; memcpy(&v, opt, 4);
                out->lease = ntohl(v);
            }
            break;
        default:
            break;
        }
        opt += olen;
    }
    return true;
}

/* ── Building and sending packets ── */

static size_t build_packet(dhcp_pkt_t *p, u8 type, u32 xid, const u8 mac[6],
                           u32 req_addr, u32 server)
{
    memset(p, 0, sizeof(*p));
    p->op    = BOOTREQUEST;
    p->htype = 1;               /* Ethernet */
    p->hlen  = 6;
    p->xid   = xid;
    p->flags = htons(0x8000);   /* ask for a broadcast reply */
    memcpy(p->chaddr, mac, 6);
    p->magic = htonl(DHCP_MAGIC);

    u8 *o = p->options;
    o = put_opt(o, OPT_MSG_TYPE, 1, &type);

    if (req_addr) o = put_opt(o, OPT_REQUESTED_IP, 4, &req_addr);
    if (server)   o = put_opt(o, OPT_SERVER_ID,    4, &server);

    /* What we are asking the server for */
    static const u8 want[] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS };
    o = put_opt(o, OPT_PARAM_LIST, sizeof(want), want);

    *o++ = OPT_END;

    return (size_t)(o - (u8 *)p);
}

static long send_bcast(int fd, const void *buf, size_t len)
{
    sockaddr_in_t dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_SERVER_PORT),
        .sin_addr   = 0xFFFFFFFFu,          /* 255.255.255.255 */
    };
    return lp_sendto(fd, buf, len, 0, &dst, sizeof(dst));
}

/* Wait for a reply of the wanted type. The timeout is a socket option. */
static bool wait_reply(int fd, u32 xid, u8 want_type, lease_t *out)
{
    dhcp_pkt_t pkt;

    for (int tries = 0; tries < 8; tries++) {
        long n = lp_recvfrom(fd, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < 0)
            return false;                    /* timed out, or an error */
        if ((size_t)n < BOOTP_FIXED_LEN + 4)
            continue;                        /* too short */
        if (pkt.op != BOOTREPLY || pkt.xid != xid)
            continue;                        /* not a reply to our request */
        if (ntohl(pkt.magic) != DHCP_MAGIC)
            continue;

        u8 type = 0;
        size_t optlen = (size_t)n - BOOTP_FIXED_LEN - 4;
        if (!parse_options(pkt.options, optlen, out, &type))
            continue;

        if (type == DHCPNAK)
            return false;
        if (type != want_type)
            continue;

        out->addr = pkt.yiaddr;
        if (!out->server)
            out->server = pkt.siaddr;
        return true;
    }
    return false;
}

static void write_resolv_conf(u32 dns_be)
{
    if (!dns_be)
        return;

    char ip[16];
    ipv4_format(dns_be, ip);

    long fd = lp_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot write /etc/resolv.conf (%ld)\n", -fd);
        return;
    }
    dprintf((int)fd, "nameserver %s\n", ip);
    lp_close((int)fd);
}

static int apply_lease(const char *ifname, const lease_t *l)
{
    char a[16], m[16], g[16];
    ipv4_format(l->addr, a);
    ipv4_format(l->mask, m);
    ipv4_format(l->router, g);

    long rc = net_set_addr(ifname, l->addr);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot set the address (%ld)\n", -rc);
        return 1;
    }
    if (l->mask) {
        rc = net_set_netmask(ifname, l->mask);
        if (rc < 0)
            dprintf(STDERR_FILENO, "dhcp: cannot set the netmask (%ld)\n", -rc);
    }
    if (l->router) {
        rc = net_add_default_route(ifname, l->router);
        if (rc < 0)
            dprintf(STDERR_FILENO, "dhcp: cannot add the default route (%ld)\n", -rc);
    }

    write_resolv_conf(l->dns);

    printf("dhcp: %s  address %s  netmask %s  gateway %s  lease %us\n",
           ifname, a, m, l->router ? g : "(none)", l->lease);
    return 0;
}

/* One full exchange: DISCOVER, OFFER, REQUEST, ACK. Returns 0 when an
 * address was applied.
 *
 * `known` is the address we already hold, or 0. A renewal names it in
 * the REQUEST, which is how a server knows to give the same one back -
 * and getting the same address back is the whole point: everything that
 * reaches this machine does so by the address the router handed out. */
static int get_lease(const char *ifname, u32 known, lease_t *out)
{
    u8 mac[6];
    if (net_if_hwaddr(ifname, mac) < 0) {
        dprintf(STDERR_FILENO,
                "dhcp: cannot read the MAC address of %s\n", ifname);
        return 1;
    }

    if (net_if_up(ifname) < 0)
        dprintf(STDERR_FILENO,
                "dhcp: could not bring %s up (trying anyway)\n", ifname);

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot create a socket (%ld)\n", -fd);
        return 1;
    }

    int one = 1;
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                  (u32)strlen(ifname) + 1);

    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t me = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_CLIENT_PORT),
        .sin_addr   = 0,
    };
    if (lp_bind((int)fd, &me, sizeof(me)) < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot bind port %d\n", DHCP_CLIENT_PORT);
        lp_close((int)fd);
        return 1;
    }

    u32 xid = 0;
    if (lp_getrandom(&xid, sizeof(xid), 0) != (long)sizeof(xid) || xid == 0)
        xid = (u32)lp_getpid() * 2654435761u;   /* fallback */

    dhcp_pkt_t pkt;
    lease_t lease;

    for (int attempt = 1; attempt <= 4; attempt++) {
        memset(&lease, 0, sizeof(lease));

        size_t len = build_packet(&pkt, DHCPDISCOVER, xid, mac, known, 0);
        if (send_bcast((int)fd, &pkt, len) < 0) {
            dprintf(STDERR_FILENO, "dhcp: sending DISCOVER failed\n");
            lp_sleep_ms(1000);
            continue;
        }

        if (!wait_reply((int)fd, xid, DHCPOFFER, &lease)) {
            printf("dhcp: no reply (%d/4)\n", attempt);
            continue;
        }

        len = build_packet(&pkt, DHCPREQUEST, xid, mac, lease.addr, lease.server);
        if (send_bcast((int)fd, &pkt, len) < 0)
            continue;

        lease_t ack;
        memset(&ack, 0, sizeof(ack));
        if (!wait_reply((int)fd, xid, DHCPACK, &ack)) {
            printf("dhcp: no ACK (%d/4)\n", attempt);
            continue;
        }

        if (!ack.mask)   ack.mask   = lease.mask;
        if (!ack.router) ack.router = lease.router;
        if (!ack.dns)    ack.dns    = lease.dns;

        lp_close((int)fd);
        if (out)
            *out = ack;
        return apply_lease(ifname, &ack);
    }

    lp_close((int)fd);
    dprintf(STDERR_FILENO, "dhcp: got no address on %s\n", ifname);
    return 1;
}

/* ── Staying on the network ───────────────────────────────────────────
 *
 * A lease is a loan with a deadline. A home router usually lends an
 * address for a day or a week, and expects to be asked again before
 * then. Ask, and it hands back the same address; do not ask, and it
 * takes the address back and eventually gives it to something else.
 *
 * A board that only asks once at boot therefore works perfectly for
 * days and then, silently, is not on the network any more. Nothing here
 * notices: the machine has not crashed, so the watchdog is content;
 * memory and temperature are fine, so guard is content. It is the one
 * failure with no automatic recovery at all - and this is a system whose
 * entire purpose is being left alone for months.
 *
 * The protocol says renew at half the lease and rebind at seven eighths.
 * We do the first: at half time, ask again. That is the part that keeps
 * the address, and the rest of the state machine exists for networks
 * with several servers, which a home router is not.
 *
 * If the renewal fails we retry, backing off, and keep the address in
 * the meantime - it usually still works, because the router has not
 * handed it to anyone else yet. Giving it up early would guarantee the
 * outage we are trying to avoid.
 */
/* The interfaces this board can have, in the order worth trying:
 *   wlan0  the built-in WiFi
 *   eth0   a USB Ethernet adapter
 *   usb0   the gadget port, when it is plugged into a PC
 * Only one of them is usually there. */
static const char *IFACES[] = { "wlan0", "eth0", "usb0", NULL };

static int run_daemon(const char *given)
{
    lease_t held;
    memset(&held, 0, sizeof(held));

    char ifbuf[16];
    const char *ifname = given;

    /* With no interface named, find the one that answers. A service
     * line in /etc/services cannot know which of the three this
     * particular board has. */
    if (!ifname) {
        for (int i = 0; IFACES[i]; i++) {
            if (get_lease(IFACES[i], 0, &held) == 0) {
                strlcpy(ifbuf, IFACES[i], sizeof(ifbuf));
                ifname = ifbuf;
                break;
            }
        }
        if (!ifname) {
            /* Nothing answered. Keep the daemon alive and keep trying:
             * WiFi may associate a minute from now, and a board that
             * gave up at boot would stay off the network for good. */
            dprintf(STDERR_FILENO,
                    "dhcp: no interface answered - retrying every 60s\n");
            for (;;) {
                lp_sleep_ms(60000);
                for (int i = 0; IFACES[i]; i++) {
                    if (get_lease(IFACES[i], 0, &held) == 0) {
                        strlcpy(ifbuf, IFACES[i], sizeof(ifbuf));
                        ifname = ifbuf;
                        break;
                    }
                }
                if (ifname)
                    break;
            }
        }
    } else if (get_lease(ifname, 0, &held) != 0) {
        dprintf(STDERR_FILENO,
                "dhcp: no address yet on %s - will keep trying\n", ifname);
    }

    for (;;) {
        u32 lease_secs = held.lease ? held.lease : 600;

        /* Half the lease, and never longer than a day: a router that
         * offers a week-long lease is not a reason to go a week without
         * checking that the network still works. */
        u32 wait = lease_secs / 2;
        if (wait < 30)    wait = 30;
        if (wait > 43200) wait = 43200;

        for (u32 slept = 0; slept < wait; slept += 10)
            lp_sleep_ms(10000);

        lease_t fresh;
        memset(&fresh, 0, sizeof(fresh));

        if (get_lease(ifname, held.addr, &fresh) == 0) {
            if (held.addr && fresh.addr != held.addr) {
                char before[16], after[16];
                ipv4_format(held.addr, before);
                ipv4_format(fresh.addr, after);
                printf("dhcp: the address changed: %s -> %s\n", before, after);
            }
            held = fresh;
            continue;
        }

        /* The renewal failed. Keep what we have and try again sooner -
         * the address is very likely still ours. */
        dprintf(STDERR_FILENO,
                "dhcp: could not renew on %s - keeping the address and"
                " retrying\n", ifname);
        held.lease = held.lease > 120 ? held.lease / 2 : 120;
    }
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    bool daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: dhcp [-d] <interface>\n");
            printf("  -d  stay running and renew the lease before it\n");
            printf("      expires. Without this the address is held\n");
            printf("      until the router takes it back, and the board\n");
            printf("      falls off the network with nothing to notice.\n");
            return 0;
        } else if (!ifname) {
            ifname = argv[i];
        }
    }

    if (daemon)
        return run_daemon(ifname);       /* NULL means "find one" */

    return get_lease(ifname ? ifname : "wlan0", 0, NULL);
}
