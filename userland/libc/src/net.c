/* net.c - 소켓 래퍼와 인터페이스 설정.
 *
 * 인터페이스 설정은 netlink 대신 옛 ioctl 방식을 쓴다. IPv4 주소·넷마스크·
 * 플래그·기본 경로만 필요한데, 그 정도는 ioctl 로 충분하고 코드가 몇 배
 * 짧다. netlink 는 IPv6 나 여러 주소를 다룰 때 필요해진다. */
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

/* ── 인터페이스 ioctl ─────────────────────────────────────────────
 *
 * struct ifreq 는 이름 16바이트 + 유니온이다. 유니온 안에 sockaddr,
 * 플래그, 인덱스 등이 겹쳐 있다. 커널 헤더를 가져오지 않고 직접 배치한다.
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

/* AF_INET 소켓 하나를 열어 ioctl 을 건다. 소켓은 통신용이 아니라
 * ioctl 을 태울 통로일 뿐이다. */
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

    /* ifr_hwaddr 는 sockaddr: 앞 2바이트가 sa_family, 그 다음이 데이터 */
    memcpy(mac, r + IFR_UNION + 2, 6);
    return 0;
}

/* 주소/넷마스크 설정은 유니온 자리에 sockaddr_in 을 얹는다 */
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

long net_set_addr(const char *ifname, u32 addr_be)
{
    return if_set_inaddr(ifname, SIOCSIFADDR, addr_be);
}

long net_set_netmask(const char *ifname, u32 mask_be)
{
    return if_set_inaddr(ifname, SIOCSIFNETMASK, mask_be);
}

/* struct rtentry - 경로 추가용. 커널 배치를 직접 옮긴다.
 *   0   unsigned long rt_pad1
 *   8   struct sockaddr rt_dst      (16바이트)
 *   24  struct sockaddr rt_gateway  (16바이트)
 *   40  struct sockaddr rt_genmask  (16바이트)
 *   56  short rt_flags
 *   ... 나머지는 0 이면 된다
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

    /* 기본 경로: 목적지 0.0.0.0, 넷마스크 0.0.0.0, 게이트웨이 지정 */
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

/* ── 주소 문자열 ──────────────────────────────────────────────── */

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

    /* 네트워크 바이트 순서 = 첫 옥텟이 최하위 바이트에 오도록 */
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
