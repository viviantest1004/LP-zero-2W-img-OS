/* dhcp - 최소 DHCP 클라이언트 (RFC 2131).
 *
 * 왜 직접 만드나: DHCP 는 암호가 없는 단순한 UDP 프로토콜이다. SSH 처럼
 * 직접 구현하면 위험한 물건이 아니라서, 외부 의존을 하나 줄일 수 있다.
 * 전체가 400줄이 안 되고 udhcpc 나 dhcpcd 를 가져오는 것보다 작다.
 *
 * 흐름:
 *   DISCOVER (브로드캐스트)  ->  OFFER    (서버가 주소를 제안)
 *   REQUEST  (브로드캐스트)  ->  ACK      (확정)
 * 확정되면 인터페이스에 주소/넷마스크를 설정하고 기본 경로를 추가한 뒤
 * /etc/resolv.conf 를 쓴다.
 *
 * 다루지 않는 것: 임대 갱신(renew), DECLINE, 여러 서버 중 고르기.
 * 가정용 공유기 한 대에 붙는 데는 필요 없다.
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

/* 옵션 53 값 */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

/* 옵션 번호 */
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_LIST   55
#define OPT_END         255

#define BOOTP_FIXED_LEN  236        /* 옵션 앞까지의 고정 길이 */
#define PKT_SIZE         576        /* RFC 최소 DHCP 메시지 크기 */

/* BOOTP/DHCP 헤더. 커널이 아니라 네트워크 배치라 패킹이 필요하다. */
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
    u32 addr;       /* 받은 주소 (네트워크 순서) */
    u32 mask;
    u32 router;
    u32 dns;
    u32 server;
    u32 lease;      /* 초 (호스트 순서) */
} lease_t;

/* ── 옵션 조립 ── */

static u8 *put_opt(u8 *p, u8 code, u8 len, const void *val)
{
    *p++ = code;
    *p++ = len;
    if (len) { memcpy(p, val, len); p += len; }
    return p;
}

/* ── 옵션 파싱 ──
 * 길이 필드를 그대로 믿으면 조작된 패킷에 버퍼 밖을 읽는다.
 * 항상 끝 경계와 대조한다. */
static bool parse_options(const u8 *opt, size_t len, lease_t *out, u8 *msg_type)
{
    const u8 *end = opt + len;
    *msg_type = 0;

    while (opt < end) {
        u8 code = *opt++;

        if (code == 0) continue;            /* 패딩 */
        if (code == OPT_END) break;
        if (opt >= end) return false;       /* 길이 바이트가 없다 */

        u8 olen = *opt++;
        if (opt + olen > end) return false; /* 값이 버퍼를 넘는다 */

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

/* ── 패킷 조립/송신 ── */

static size_t build_packet(dhcp_pkt_t *p, u8 type, u32 xid, const u8 mac[6],
                           u32 req_addr, u32 server)
{
    memset(p, 0, sizeof(*p));
    p->op    = BOOTREQUEST;
    p->htype = 1;               /* 이더넷 */
    p->hlen  = 6;
    p->xid   = xid;
    p->flags = htons(0x8000);   /* 브로드캐스트로 답해 달라 */
    memcpy(p->chaddr, mac, 6);
    p->magic = htonl(DHCP_MAGIC);

    u8 *o = p->options;
    o = put_opt(o, OPT_MSG_TYPE, 1, &type);

    if (req_addr) o = put_opt(o, OPT_REQUESTED_IP, 4, &req_addr);
    if (server)   o = put_opt(o, OPT_SERVER_ID,    4, &server);

    /* 받고 싶은 정보 목록 */
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

/* 원하는 타입의 응답을 기다린다. 타임아웃은 소켓 옵션으로 건다. */
static bool wait_reply(int fd, u32 xid, u8 want_type, lease_t *out)
{
    dhcp_pkt_t pkt;

    for (int tries = 0; tries < 8; tries++) {
        long n = lp_recvfrom(fd, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < 0)
            return false;                    /* 타임아웃 또는 오류 */
        if ((size_t)n < BOOTP_FIXED_LEN + 4)
            continue;                        /* 너무 짧다 */
        if (pkt.op != BOOTREPLY || pkt.xid != xid)
            continue;                        /* 내 요청이 아니다 */
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
        dprintf(STDERR_FILENO, "dhcp: /etc/resolv.conf 를 쓸 수 없습니다 (%ld)\n", -fd);
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
        dprintf(STDERR_FILENO, "dhcp: 주소 설정 실패 (%ld)\n", -rc);
        return 1;
    }
    if (l->mask) {
        rc = net_set_netmask(ifname, l->mask);
        if (rc < 0)
            dprintf(STDERR_FILENO, "dhcp: 넷마스크 설정 실패 (%ld)\n", -rc);
    }
    if (l->router) {
        rc = net_add_default_route(ifname, l->router);
        if (rc < 0)
            dprintf(STDERR_FILENO, "dhcp: 기본 경로 추가 실패 (%ld)\n", -rc);
    }

    write_resolv_conf(l->dns);

    printf("dhcp: %s  주소 %s  넷마스크 %s  게이트웨이 %s  임대 %u초\n",
           ifname, a, m, l->router ? g : "(없음)", l->lease);
    return 0;
}

int main(int argc, char **argv)
{
    const char *ifname = (argc > 1) ? argv[1] : "wlan0";

    u8 mac[6];
    if (net_if_hwaddr(ifname, mac) < 0) {
        dprintf(STDERR_FILENO, "dhcp: %s 의 MAC 주소를 읽을 수 없습니다\n", ifname);
        return 1;
    }

    if (net_if_up(ifname) < 0)
        dprintf(STDERR_FILENO, "dhcp: %s 를 올리지 못했습니다 (계속 시도)\n", ifname);

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: 소켓 생성 실패 (%ld)\n", -fd);
        return 1;
    }

    int one = 1;
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    /* 주소가 아직 없으므로 인터페이스에 직접 묶는다.
     * 이게 없으면 경로가 없어서 송신이 실패한다. */
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                  (u32)strlen(ifname) + 1);

    /* 응답 대기 3초. struct __kernel_timespec { s64 sec; s64 nsec; } */
    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t me = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_CLIENT_PORT),
        .sin_addr   = 0,
    };
    if (lp_bind((int)fd, &me, sizeof(me)) < 0) {
        dprintf(STDERR_FILENO, "dhcp: 포트 %d 바인드 실패\n", DHCP_CLIENT_PORT);
        lp_close((int)fd);
        return 1;
    }

    u32 xid = 0;
    if (lp_getrandom(&xid, sizeof(xid), 0) != (long)sizeof(xid) || xid == 0)
        xid = (u32)lp_getpid() * 2654435761u;   /* 폴백 */

    dhcp_pkt_t pkt;
    lease_t lease;

    /* DISCOVER 를 몇 번 재시도한다. 무선은 연결 직후 잠깐 못 받을 수 있다. */
    for (int attempt = 1; attempt <= 4; attempt++) {
        memset(&lease, 0, sizeof(lease));

        size_t len = build_packet(&pkt, DHCPDISCOVER, xid, mac, 0, 0);
        if (send_bcast((int)fd, &pkt, len) < 0) {
            dprintf(STDERR_FILENO, "dhcp: DISCOVER 송신 실패\n");
            lp_sleep_ms(1000);
            continue;
        }

        if (!wait_reply((int)fd, xid, DHCPOFFER, &lease)) {
            printf("dhcp: 응답 없음 (%d/4)\n", attempt);
            continue;
        }

        len = build_packet(&pkt, DHCPREQUEST, xid, mac, lease.addr, lease.server);
        if (send_bcast((int)fd, &pkt, len) < 0)
            continue;

        lease_t ack;
        memset(&ack, 0, sizeof(ack));
        if (!wait_reply((int)fd, xid, DHCPACK, &ack)) {
            printf("dhcp: ACK 없음 (%d/4)\n", attempt);
            continue;
        }

        /* ACK 에 없는 값은 OFFER 것을 쓴다 */
        if (!ack.mask)   ack.mask   = lease.mask;
        if (!ack.router) ack.router = lease.router;
        if (!ack.dns)    ack.dns    = lease.dns;

        lp_close((int)fd);
        return apply_lease(ifname, &ack);
    }

    lp_close((int)fd);
    dprintf(STDERR_FILENO, "dhcp: %s 에서 주소를 받지 못했습니다\n", ifname);
    return 1;
}
