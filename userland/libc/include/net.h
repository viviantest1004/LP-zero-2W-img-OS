/* net.h - 소켓과 네트워크 인터페이스 설정.
 *
 * IPv4 만 다룬다. 커널이 TCP/IP 를 처리하므로 우리가 만들 것은
 * 소켓 래퍼와 인터페이스 설정(ioctl) 뿐이다. */
#ifndef _LP_NET_H
#define _LP_NET_H

#include "types.h"

/* 주소 패밀리 / 소켓 타입 */
#define AF_INET         2
#define AF_PACKET      17
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

#define IPPROTO_IP      0
#define IPPROTO_UDP    17

/* setsockopt */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BROADCAST    6
#define SO_BINDTODEVICE 25
#define SO_RCVTIMEO_NEW 66

/* 인터페이스 ioctl */
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCSIFADDR     0x8916
#define SIOCSIFNETMASK  0x891C
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCADDRT       0x890B

#define IFF_UP          0x1
#define IFF_RUNNING     0x40

#define IFNAMSIZ        16

/* struct sockaddr_in - 커널이 기대하는 배치 그대로 */
typedef struct {
    u16 sin_family;
    u16 sin_port;       /* 네트워크 바이트 순서 */
    u32 sin_addr;       /* 네트워크 바이트 순서 */
    u8  sin_zero[8];
} sockaddr_in_t;

/* 바이트 순서 변환. AArch64 는 리틀엔디언이므로 실제로 뒤집는다. */
static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline u32 ntohl(u32 v) { return htonl(v); }

/* ── 소켓 ── */
long lp_socket(int family, int type, int proto);
long lp_bind(int fd, const void *addr, u32 addrlen);
long lp_connect(int fd, const void *addr, u32 addrlen);
long lp_sendto(int fd, const void *buf, size_t n, int flags,
               const void *addr, u32 addrlen);
long lp_recvfrom(int fd, void *buf, size_t n, int flags,
                 void *addr, u32 *addrlen);
long lp_setsockopt(int fd, int level, int opt, const void *val, u32 len);

/* ── 인터페이스 설정 ── */
long net_if_up(const char *ifname);
long net_if_down(const char *ifname);
bool net_if_is_up(const char *ifname);
long net_if_index(const char *ifname, int *index_out);
long net_if_hwaddr(const char *ifname, u8 mac[6]);
long net_set_addr(const char *ifname, u32 addr_be);      /* 네트워크 순서 */
long net_set_netmask(const char *ifname, u32 mask_be);
long net_add_default_route(const char *ifname, u32 gw_be);

/* "192.168.0.1" -> 네트워크 순서 u32. 실패 시 false. */
bool ipv4_parse(const char *s, u32 *out_be);
/* 네트워크 순서 u32 -> "192.168.0.1". buf 는 16바이트 이상. */
void ipv4_format(u32 addr_be, char *buf);

#endif /* _LP_NET_H */
