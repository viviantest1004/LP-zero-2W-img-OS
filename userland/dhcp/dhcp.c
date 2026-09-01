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

int main(int argc, char **argv)
{
    const char *ifname = (argc > 1) ? argv[1] : "wlan0";

    u8 mac[6];
    if (net_if_hwaddr(ifname, mac) < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot read the MAC address of %s\n", ifname);
        return 1;
    }

    if (net_if_up(ifname) < 0)
        dprintf(STDERR_FILENO, "dhcp: could not bring %s up (trying anyway)\n", ifname);

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot create a socket (%ld)\n", -fd);
        return 1;
    }

    int one = 1;
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    /* We have no address yet, so bind straight to the interface.
     * Without this there is no route and sending fails. */
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                  (u32)strlen(ifname) + 1);

    /* Wait 3s for a reply. struct __kernel_timespec { s64 sec; s64 nsec; } */
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

    /* Retry DISCOVER a few times. WiFi can miss replies just after joining. */
    for (int attempt = 1; attempt <= 4; attempt++) {
        memset(&lease, 0, sizeof(lease));

        size_t len = build_packet(&pkt, DHCPDISCOVER, xid, mac, 0, 0);
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

        /* Anything the ACK left out, take from the OFFER. */
        if (!ack.mask)   ack.mask   = lease.mask;
        if (!ack.router) ack.router = lease.router;
        if (!ack.dns)    ack.dns    = lease.dns;

        lp_close((int)fd);
        return apply_lease(ifname, &ack);
    }

    lp_close((int)fd);
    dprintf(STDERR_FILENO, "dhcp: got no address on %s\n", ifname);
    return 1;
}
