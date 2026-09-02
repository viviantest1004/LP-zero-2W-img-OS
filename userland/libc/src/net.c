/* net.c - socket wrappers and interface configuration.
 *
 * Interfaces are configured through the old ioctl interface rather than
 * netlink. We only need an IPv4 address, a netmask, the flags and a
 * default route; ioctl covers that in a fraction of the code. netlink
 * starts to matter with IPv6 or several addresses per interface. */
#include "net.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"

long lp_socket(int family, int type, int proto)
{
    return sys_call3(SYS_socket, family, type, proto);
}

long lp_bind(int fd, const void *addr, u32 addrlen)
{
    return sys_call3(SYS_bind, fd, (long)addr, (long)addrlen);
}

long lp_connect(int fd, const void *addr, u32 addrlen)
{
    return sys_call3(SYS_connect, fd, (long)addr, (long)addrlen);
}

long lp_sendto(int fd, const void *buf, size_t n, int flags,
               const void *addr, u32 addrlen)
{
    return sys_call6(SYS_sendto, fd, (long)buf, (long)n, flags,
                     (long)addr, (long)addrlen);
}

long lp_recvfrom(int fd, void *buf, size_t n, int flags,
                 void *addr, u32 *addrlen)
{
    return sys_call6(SYS_recvfrom, fd, (long)buf, (long)n, flags,
                     (long)addr, (long)addrlen);
}

long lp_setsockopt(int fd, int level, int opt, const void *val, u32 len)
{
    return sys_call5(SYS_setsockopt, fd, level, opt, (long)val, (long)len);
}

/* ── Interface ioctls ─────────────────────────────────────────────
 *
 * struct ifreq is a 16-byte name followed by a union. The union overlays
 * a sockaddr, the flags, the index and more. We lay it out ourselves
 * rather than pulling in kernel headers.
 *
 *   offset 0..15   ifr_name
 *   offset 16..    ifr_addr / ifr_flags / ifr_ifindex / ifr_hwaddr
 */
#define IFREQ_SIZE   40
#define IFR_UNION    16

typedef u8 ifreq_t[IFREQ_SIZE];

static void ifreq_init(ifreq_t r, const char *ifname)
{
    memset(r, 0, IFREQ_SIZE);
    strlcpy((char *)r, ifname, IFNAMSIZ);
}

/* Open an AF_INET socket to carry the ioctl. The socket is never used
 * to communicate - it is just the handle the ioctl rides on. */
static long if_ioctl(unsigned long req, ifreq_t r)
{
    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fd;

    long rc = sys_call3(SYS_ioctl, (long)fd, (long)req, (long)r);
    lp_close((int)fd);
    return rc;
}

static long if_set_flags(const char *ifname, u16 set, u16 clear)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFFLAGS, r);
    if (rc < 0)
        return rc;

    u16 flags = *(u16 *)(r + IFR_UNION);
    flags = (u16)((flags | set) & ~clear);
    *(u16 *)(r + IFR_UNION) = flags;

    return if_ioctl(SIOCSIFFLAGS, r);
}

long net_if_up(const char *ifname)
{
    return if_set_flags(ifname, IFF_UP | IFF_RUNNING, 0);
}

long net_if_down(const char *ifname)
{
    return if_set_flags(ifname, 0, IFF_UP);
}

bool net_if_is_up(const char *ifname)
{
    ifreq_t r;
    ifreq_init(r, ifname);
    if (if_ioctl(SIOCGIFFLAGS, r) < 0)
        return false;
    return (*(u16 *)(r + IFR_UNION) & IFF_UP) != 0;
}

long net_if_index(const char *ifname, int *index_out)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFINDEX, r);
    if (rc < 0)
        return rc;

    if (index_out)
        *index_out = *(int *)(r + IFR_UNION);
    return 0;
}

long net_if_hwaddr(const char *ifname, u8 mac[6])
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFHWADDR, r);
    if (rc < 0)
        return rc;

    /* ifr_hwaddr is a sockaddr: 2 bytes of sa_family, then the data. */
    memcpy(mac, r + IFR_UNION + 2, 6);
    return 0;
}

/* Setting an address or netmask puts a sockaddr_in in the union. */
static long if_set_inaddr(const char *ifname, unsigned long req, u32 addr_be)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    sockaddr_in_t *sa = (sockaddr_in_t *)(r + IFR_UNION);
    sa->sin_family = AF_INET;
    sa->sin_port   = 0;
    sa->sin_addr   = addr_be;

    return if_ioctl(req, r);
}

long net_get_addr(const char *ifname, u32 *addr_be)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFADDR, r);
    if (rc < 0)
        return rc;

    sockaddr_in_t *sa = (sockaddr_in_t *)(r + IFR_UNION);
    if (addr_be) *addr_be = sa->sin_addr;
    return 0;
}

long net_set_addr(const char *ifname, u32 addr_be)
{
    return if_set_inaddr(ifname, SIOCSIFADDR, addr_be);
}

long net_set_netmask(const char *ifname, u32 mask_be)
{
    return if_set_inaddr(ifname, SIOCSIFNETMASK, mask_be);
}

/* struct rtentry, for adding a route. The kernel's layout, by hand.
 *   0   unsigned long rt_pad1
 *   8   struct sockaddr rt_dst      (16 bytes)
 *   24  struct sockaddr rt_gateway  (16 bytes)
 *   40  struct sockaddr rt_genmask  (16 bytes)
 *   56  short rt_flags
 *   ... the rest can stay zero
 */
#define RTENTRY_SIZE   120
#define RT_DST_OFF       8
#define RT_GATEWAY_OFF  24
#define RT_GENMASK_OFF  40
#define RT_FLAGS_OFF    56
#define RT_DEV_OFF      72      /* char *rt_dev */

#define RTF_UP       0x0001
#define RTF_GATEWAY  0x0002

long net_add_default_route(const char *ifname, u32 gw_be)
{
    u8 rt[RTENTRY_SIZE];
    memset(rt, 0, sizeof(rt));

    /* The default route: destination 0.0.0.0, netmask 0.0.0.0, via the gateway. */
    sockaddr_in_t *dst  = (sockaddr_in_t *)(rt + RT_DST_OFF);
    sockaddr_in_t *gw   = (sockaddr_in_t *)(rt + RT_GATEWAY_OFF);
    sockaddr_in_t *mask = (sockaddr_in_t *)(rt + RT_GENMASK_OFF);

    dst->sin_family  = AF_INET;
    dst->sin_addr    = 0;
    mask->sin_family = AF_INET;
    mask->sin_addr   = 0;
    gw->sin_family   = AF_INET;
    gw->sin_addr     = gw_be;

    *(u16 *)(rt + RT_FLAGS_OFF) = RTF_UP | RTF_GATEWAY;
    *(const char **)(rt + RT_DEV_OFF) = ifname;

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fd;

    long rc = sys_call3(SYS_ioctl, (long)fd, SIOCADDRT, (long)rt);
    lp_close((int)fd);
    return rc;
}

/* ── Address strings ───────────────────────────────────────────── */

bool ipv4_parse(const char *s, u32 *out_be)
{
    u32 octets[4];

    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;

        u32 v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s++ - '0');
            if (v > 255)
                return false;
        }
        octets[i] = v;

        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s != '\0')
        return false;

    /* Network byte order: the first octet ends up in the lowest byte. */
    *out_be = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

void ipv4_format(u32 addr_be, char *buf)
{
    u8 *o = (u8 *)&addr_be;
    char *p = buf;

    for (int i = 0; i < 4; i++) {
        u32 v = o[i];
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
        *p++ = (char)('0' + v % 10);
        if (i < 3) *p++ = '.';
    }
    *p = '\0';
}

/* ── Resolving a name ─────────────────────────────────────────────────
 *
 * A full resolver is a large thing: caching, search domains, CNAME
 * chains, TCP fallback for long answers, IPv6. None of that is needed to
 * turn one host name into one address, which is the only question anyone
 * on this machine asks. So this sends a single A query to the first
 * nameserver in /etc/resolv.conf and reads the first A record back.
 *
 * A numeric address is returned as it stands, so every caller can take a
 * name or an address without checking which it has. */

/* First nameserver from /etc/resolv.conf. 0 if we cannot read one. */
static u32 read_nameserver(void)
{
    char buf[512];
    long fd = lp_open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    /* Look for a "nameserver 1.2.3.4" line. */
    for (char *p = buf; *p; ) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';

        static const char key[] = "nameserver";
        if (strncmp(line, key, sizeof(key) - 1) != 0)
            continue;
        char *ip = line + sizeof(key) - 1;
        while (*ip == ' ' || *ip == '\t') ip++;

        u32 addr = 0;
        if (ipv4_parse(ip, &addr))
            return addr;        /* network byte order */
    }
    return 0;
}

/* Encode a name for a DNS query: "a.b.com" -> 1'a' 1'b' 3'c''o''m' 0
 * Returns the bytes written, or 0 if it would not fit. */
static size_t encode_name(u8 *out, size_t cap, const char *host)
{
    size_t o = 0;
    const char *p = host;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        size_t len = (size_t)(dot - p);
        if (len == 0 || len > 63 || o + len + 1 >= cap)
            return 0;
        out[o++] = (u8)len;
        memcpy(out + o, p, len);
        o += len;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    if (o + 1 > cap)
        return 0;
    out[o++] = 0;
    return o;
}

/* Name to IPv4. The address in network order, or 0 on failure. */
u32 net_resolve(const char *host)
{
    /* Already a numeric address? Use it as is. */
    u32 direct = 0;
    if (ipv4_parse(host, &direct))
        return direct;

    u32 ns = read_nameserver();
    if (ns == 0)
        return 0;               /* no nameserver: run dhcp first */

    u8     query[512];
    size_t qlen = 0;

    /* 12-byte header: ID, flags (recursion desired), one question. */
    u16 id = (u16)(lp_getpid() & 0xFFFF);
    query[qlen++] = (u8)(id >> 8);   query[qlen++] = (u8)id;
    query[qlen++] = 0x01;            query[qlen++] = 0x00;   /* RD */
    query[qlen++] = 0x00;            query[qlen++] = 0x01;   /* QDCOUNT=1 */
    query[qlen++] = 0x00;            query[qlen++] = 0x00;
    query[qlen++] = 0x00;            query[qlen++] = 0x00;
    query[qlen++] = 0x00;            query[qlen++] = 0x00;

    size_t nlen = encode_name(query + qlen, sizeof(query) - qlen - 4, host);
    if (nlen == 0)
        return 0;
    qlen += nlen;
    query[qlen++] = 0x00; query[qlen++] = 0x01;   /* QTYPE = A */
    query[qlen++] = 0x00; query[qlen++] = 0x01;   /* QCLASS = IN */

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    /* struct __kernel_sock_timeval { s64 tv_sec; s64 tv_usec; } */
    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    to.sin_addr   = ns;

    u32 result = 0;
    if (lp_sendto((int)fd, query, qlen, 0, &to, sizeof(to)) > 0) {
        u8   resp[512];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);

        /* Skip the header and the question, then walk the answers. A name
         * compression pointer (0xC0) is 2 bytes; otherwise skip the label. */
        if (n > 12 && resp[0] == (u8)(id >> 8) && resp[1] == (u8)id) {
            u16 ancount = (u16)((resp[6] << 8) | resp[7]);
            long off = 12;

            /* The name in the question section */
            while (off < n && resp[off] != 0) {
                if ((resp[off] & 0xC0) == 0xC0) { off += 2; goto qtype; }
                off += resp[off] + 1;
            }
            off += 1;
qtype:
            off += 4;               /* QTYPE + QCLASS */

            for (u16 i = 0; i < ancount && off + 12 <= n; i++) {
                if ((resp[off] & 0xC0) == 0xC0) {
                    off += 2;
                } else {
                    while (off < n && resp[off] != 0) off += resp[off] + 1;
                    off += 1;
                }
                if (off + 10 > n) break;
                u16 type   = (u16)((resp[off] << 8) | resp[off + 1]);
                u16 rdlen  = (u16)((resp[off + 8] << 8) | resp[off + 9]);
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= n) {
                    memcpy(&result, resp + off, 4);   /* already network order */
                    break;
                }
                off += rdlen;
            }
        }
    }

    lp_close((int)fd);
    return result;
}
