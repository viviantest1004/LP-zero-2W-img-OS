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
#include "stdio.h"
#include "stdlib.h"

/* Mozilla's root certificates, put on the data partition by
 * tools/mksdcard.sh. Only used for https:// - see net_https_get. */
#define CA_BUNDLE "/data/ssl/cert.pem"

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

    /* 12-byte header: ID, flags (recursion desired), one question.
     *
     * The ID used to be the pid. That is the only thing distinguishing a
     * real answer from a forged one here, and /etc/rc caps pid_max at
     * 4096, so the search space was under twelve bits - and a daemon
     * started at boot has a small, stable pid across reboots. Anything
     * that could get a UDP packet to the port could answer first with
     * whatever address it liked, for every name this machine looks up:
     * ntp, beacon, pkg, and update, which has no signature check.
     *
     * getrandom, with the pid only as a fallback if that syscall is not
     * there. */
    u16 id;
    if (lp_getrandom(&id, sizeof id, 0) != (long)sizeof id)
        id = (u16)((lp_getpid() * 2654435761u) >> 13);
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

    /* connect() the socket so the kernel drops datagrams from anyone
     * but the nameserver we asked. Without it the reply was accepted
     * from any source address at all. */
    if (lp_connect((int)fd, &to, sizeof(to)) < 0) {
        lp_close((int)fd);
        return 0;
    }

    u32 result = 0;
    if (lp_sendto((int)fd, query, qlen, 0, &to, sizeof(to)) > 0) {
        u8   resp[512];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);

        /* Skip the header and the question, then walk the answers. A name
         * compression pointer (0xC0) is 2 bytes; otherwise skip the label. */
        /* The ID must match, it must be a response (QR bit), and the
         * question echoed back must be the one we asked - a reply that
         * answers a different name is not an answer to this query. */
        bool same_question =
            n > 12 + (long)nlen + 4 &&
            memcmp(resp + 12, query + 12, nlen + 4) == 0;

        if (n > 12 && resp[0] == (u8)(id >> 8) && resp[1] == (u8)id &&
            (resp[2] & 0x80) && same_question) {
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

/* ── Fetching a file over HTTP ────────────────────────────────────────
 *
 * GET, one connection, no redirects, no chunked encoding, no HTTPS.
 *
 * That last one is the real limit and worth being plain about: there is
 * no TLS in this userland, so this cannot talk to an https:// URL at
 * all. What it is for is a file on a server you already trust, checked
 * afterwards against a hash you got some other way. For anything else,
 * python3 has a full TLS stack and is on /data.
 *
 * HTTP/1.0 with Connection: close, because then the end of the body is
 * the end of the connection and there is no chunked encoding to decode. */

/* Split "http://host[:port]/path" apart. false if it is not one. */
static bool url_split(const char *url, char *host, size_t hsize,
                      int *port, char *path, size_t psize)
{
    static const char scheme[] = "http://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0)
        return false;

    const char *p = url + sizeof(scheme) - 1;
    size_t i = 0;
    *port = 80;

    while (*p && *p != '/' && *p != ':' && i < hsize - 1)
        host[i++] = *p++;
    host[i] = '\0';
    if (i == 0)
        return false;

    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && *p != '/') p++;
    }

    strlcpy(path, *p ? p : "/", psize);
    return true;
}

/* ── HTTPS ────────────────────────────────────────────────────────
 *
 * There is no TLS in this userland and there is not going to be. A
 * hand-written TLS stack is the one piece of code in a system like this
 * that is both very hard to get right and catastrophic when it is
 * wrong, and getting it wrong is silent - a certificate that is not
 * really checked looks exactly like one that is.
 *
 * CPython has a real one, with OpenSSL statically linked into it, and
 * it is already on the data partition for other reasons. So an https://
 * URL is handed to it. That costs a few seconds of interpreter startup
 * and only works on an image that carries Python, which is the honest
 * trade: the alternative is either no HTTPS at all or a bad HTTPS.
 *
 * The certificates come from /data/ssl/cert.pem - Mozilla's root list,
 * copied in by tools/build-sysroot.sh, deliberately not the build
 * machine's own bundle, which in a CI container has that CI's TLS
 * interception CA in it. */
static const char *find_python(void)
{
    static const char *candidates[] = {
        "/data/python/bin/python3.12",
        "/data/bin/python3",
        "/bin/python3",
        0
    };
    for (int i = 0; candidates[i]; i++)
        if (lp_exists(candidates[i]))
            return candidates[i];
    return 0;
}

static long net_https(const char *method, const char *url,
                      const char *body, const char *dest)
{
    const char *py = find_python();
    if (!py) {
        dprintf(STDERR_FILENO,
                "%s: https needs python3, which is not on this image.\n"
                "  This userland has no TLS of its own - see net.c.\n"
                "  An http:// URL works without it.\n", url);
        return -1;
    }
    if (!lp_exists(CA_BUNDLE)) {
        dprintf(STDERR_FILENO,
                "%s: no root certificates at %s, so nothing could be\n"
                "  checked. Refusing rather than trusting whatever answers.\n",
                url, CA_BUNDLE);
        return -1;
    }

    /* Everything the child needs is in argv; nothing is interpolated
     * into the program text, so a URL cannot become code.
     *
     * The exception handling is not decoration. Left to itself, a
     * rejected certificate comes out of Python as forty lines of
     * traceback through urllib and ssl, and the one line that says what
     * went wrong is in the middle of it. A command that failed should
     * say so in a sentence. */
    static const char *prog =
        "import os,ssl,sys,urllib.request\n"
        "m,url,ca,dest,body=sys.argv[1:6]\n"
        "n=0\n"
        "try:\n"
        "    c=ssl.create_default_context(cafile=ca)\n"
        "    d=body.encode() if body else None\n"
        "    h={'Content-Type':'application/json'} if body else {}\n"
        "    q=urllib.request.Request(url,data=d,headers=h,method=m)\n"
        "    r=urllib.request.urlopen(q,timeout=30,context=c)\n"
        "    f=open(dest,'wb') if dest!='-' else None\n"
        "    while True:\n"
        "        b=r.read(65536)\n"
        "        if not b: break\n"
        "        n+=len(b)\n"
        "        if f: f.write(b)\n"
        "    if f: f.close()\n"
        "except Exception as e:\n"
        "    m=str(e).replace('\\n',' ')\n"
        "    sys.stderr.write('https: '+m+'\\n')\n"
        "    if 'CERTIFICATE_VERIFY' in m:\n"
        "        sys.stderr.write('  checked against '+ca+'. A clock that"
        " is wrong makes every\\n  certificate look invalid, so try ntp"
        " before anything else.\\n')\n"
        "    try:\n"
        "        if dest!='-': os.unlink(dest)\n"
        "    except OSError: pass\n"
        "    sys.exit(4)\n"
        "sys.exit(0)\n";

    char *argv[] = { (char *)py, (char *)"-c", (char *)prog,
                     (char *)method, (char *)url, (char *)CA_BUNDLE,
                     (char *)(dest ? dest : "-"),
                     (char *)(body ? body : ""), 0 };

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "%s: cannot start %s\n", url, py);
        return -1;
    }
    if (pid == 0) {
        lp_execve(py, argv, environ);
        lp_exit(127);
    }

    int status = 0;
    lp_waitpid(pid, &status, 0);
    int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
    if (code != 0) {
        /* Code 4 means the child already said what was wrong. */
        if (code == 127)
            dprintf(STDERR_FILENO, "%s: could not run %s\n", url, py);
        else if (code == 3)
            dprintf(STDERR_FILENO, "%s: the server sent nothing\n", url);
        else if (code != 4)
            dprintf(STDERR_FILENO, "%s: download failed\n", url);
        return -1;
    }

    if (!dest)
        return 0;                   /* asked for nothing, got nothing */

    lp_stat_t st;
    if (lp_stat(dest, &st, true) < 0)
        return -1;
    return (long)st.size;
}

/* One function behind both net_http_get and net_http_post: the two
 * differ by a word in the request line and whether a body follows. */
static char lower_ch(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Find "Name:" in a set of HTTP headers and read the number after it.
 * Case-insensitive on the name, because header names are. Returns -1
 * when it is not there or is not a number. */
static long header_value_long(const char *head, const char *name)
{
    size_t nlen = strlen(name);

    for (const char *p = head; *p; ) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               lower_ch(p[i]) == lower_ch(name[i]))
            i++;
        if (i == nlen) {
            const char *v = p + nlen;
            while (*v == ' ' || *v == '\t') v++;
            if (*v < '0' || *v > '9')
                return -1;
            return strtol(v, NULL, 10);
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

static long http_do(const char *method, const char *url,
                    const char *body, const char *dest)
{
    char host[128], path[512];
    int  port;

    if (strncmp(url, "https://", 8) == 0)
        return net_https(method, url, body, dest);

    if (!url_split(url, host, sizeof(host), &port, path, sizeof(path))) {
        dprintf(STDERR_FILENO,
                "%s: this is not an http:// or https:// URL\n", url);
        return -1;
    }

    u32 addr = net_resolve(host);
    if (addr == 0) {
        dprintf(STDERR_FILENO, "cannot resolve %s\n", host);
        return -1;
    }

    long fd = lp_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    s64 tv[2] = { 20, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));
    /* Give up on an unanswered SYN in about twenty seconds instead of
     * two minutes. Without this, one unreachable host - or one port the
     * firewall is dropping - makes the whole program look hung. */
    int syn_tries = 3;
    lp_setsockopt((int)fd, IPPROTO_TCP, IPPROTO_TCP_SYNCNT,
                  &syn_tries, sizeof(syn_tries));

    sockaddr_in_t sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u16)port);
    sa.sin_addr   = addr;

    if (lp_connect((int)fd, &sa, sizeof(sa)) < 0) {
        dprintf(STDERR_FILENO, "cannot connect to %s:%d\n", host, port);
        lp_close((int)fd);
        return -1;
    }

    char req[768];
    int  rn;
    if (body) {
        rn = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: lpzero\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %lu\r\n"
                      "Connection: close\r\n\r\n",
                      method, path, host, (unsigned long)strlen(body));
    } else {
        rn = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: lpzero\r\n"
                      "Connection: close\r\n\r\n", method, path, host);
    }
    lp_write((int)fd, req, (size_t)rn);
    if (body)
        lp_write((int)fd, body, strlen(body));

    /* A caller that only wants to know the request arrived passes no
     * destination - a heartbeat, for instance, where the reply is
     * "200" and nothing else. */
    long out = dest ? lp_open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                    : lp_open("/dev/null", O_WRONLY, 0);
    if (out < 0) {
        dprintf(STDERR_FILENO, "cannot write %s\n", dest ? dest : "/dev/null");
        lp_close((int)fd);
        return -1;
    }

    /* Read past the headers. The blank line between them and the body may
     * land anywhere in a read, so we look for it as we go. */
    static char buf[8192];
    bool  in_body = false;
    int   match   = 0;          /* how much of \r\n\r\n we have seen */
    long  written = 0;
    int   status  = 0;
    bool  have_status = false;

    /* The header is collected before it is parsed. The status used to
     * be read as atoi(buf + 9) from whatever the first read returned,
     * which assumed the first read holds at least the status line - a
     * server is free to send it in smaller pieces, and one that wants
     * to can. Twelve bytes or fewer and have_status stayed false, which
     * skipped the status check entirely and handed a 404 page back as a
     * downloaded file; a later read and buf+9 pointed into the middle
     * of a header and gave a nonsense number. */
    char  head[512];
    int   nhead = 0;
    long  content_length = -1;
    bool  bad_read = false;

    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n < 0) { bad_read = true; break; }
        if (n == 0)
            break;

        long i = 0;
        if (!in_body) {
            for (long k = 0; k < n && nhead < (int)sizeof head - 1; k++)
                head[nhead++] = buf[k];
            head[nhead] = '\0';
            for (; i < n; i++) {
                char c = buf[i];
                if ((match == 0 || match == 2) && c == '\r')      match++;
                else if ((match == 1 || match == 3) && c == '\n') match++;
                else                                              match = (c == '\r');
                if (match == 4) { i++; in_body = true; break; }
            }

            if (in_body && !have_status) {
                /* "HTTP/1.1 200 OK" - the digits start at byte 9. */
                if (nhead > 12 && strncmp(head, "HTTP/", 5) == 0) {
                    status      = atoi(head + 9);
                    have_status = true;
                }
                content_length = header_value_long(head, "Content-Length:");
            }
        }

        if (in_body && i < n) {
            lp_write((int)out, buf + i, (size_t)(n - i));
            written += n - i;
        }
    }

    lp_close((int)out);
    lp_close((int)fd);

    if (!have_status) {
        dprintf(STDERR_FILENO,
                "%s: no HTTP status line in the reply\n", url);
        if (dest) lp_unlink(dest);
        return -1;
    }
    if (status != 200) {
        dprintf(STDERR_FILENO, "%s: the server said %d\n", url, status);
        if (dest)
            lp_unlink(dest);
        return -1;
    }

    /* Did the whole body arrive?
     *
     * The loop above ends on any read that returns 0, and a connection
     * that is reset or simply stops mid-body looks exactly like a
     * finished one. Nothing checked, so nothing downstream COULD check:
     * `update` accepted a kernel image cut off after 18% of the file,
     * called it verified, and installed it as the thing the board
     * boots. That is the one failure this machine cannot repair by
     * itself - nothing runs before the GPU firmware, and it cannot be
     * told to try a second file.
     *
     * A server that sends no Content-Length leaves nothing to compare
     * against; that is not this code's fault, but it is worth saying so
     * the caller knows the difference. */
    if (bad_read) {
        dprintf(STDERR_FILENO, "%s: the connection broke mid-transfer\n", url);
        if (dest) lp_unlink(dest);
        return -1;
    }
    if (content_length >= 0 && written != content_length) {
        dprintf(STDERR_FILENO,
                "%s: got %ld bytes, the server said %ld - discarding it\n",
                url, written, content_length);
        if (dest) lp_unlink(dest);
        return -1;
    }
    if (content_length < 0 && dest)
        dprintf(STDERR_FILENO,
                "%s: the server sent no length, so a short download"
                " cannot be detected here\n", url);

    return written;
}

long net_http_get(const char *url, const char *dest)
{
    return http_do("GET", url, NULL, dest);
}

long net_http_post(const char *url, const char *body, const char *dest)
{
    return http_do("POST", url, body ? body : "", dest);
}
