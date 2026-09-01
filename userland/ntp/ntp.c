/* ntp - 네트워크에서 시각을 받아 시스템 시계를 맞춘다.
 *
 *   ntp                     기본 서버들에 물어보고 시계를 맞춘다
 *   ntp <서버|IP>...        지정한 서버에 물어본다
 *   ntp -r                  네트워크 없이, 저장해둔 마지막 시각으로 되돌린다
 *
 * 왜 필요한가:
 *   Pi Zero 2 W 에는 배터리로 도는 시계(RTC)가 없다. 전원을 넣으면
 *   커널 시계가 1970년 1월 1일에서 시작한다. 그 상태에서 HTTPS 를 쓰면
 *   서버 인증서의 유효기간이 "아직 시작되지 않은 미래"로 보여서 검증이
 *   전부 실패한다. 시계가 맞아야 TLS 가 된다.
 *
 * 어떻게:
 *   SNTP(RFC 4330). NTP 패킷 48바이트를 UDP 123 으로 보내고 답에서
 *   transmit timestamp 를 읽는다. 정밀한 동기화(주파수 보정, 여러 서버
 *   비교)는 하지 않는다. 초 단위로 맞으면 인증서 검증에는 충분하다.
 *
 *   호스트 이름은 /etc/resolv.conf 의 네임서버에 A 레코드를 물어
 *   직접 해석한다. 우리 libc 에는 리졸버가 없다.
 *
 * 맞춘 시각은 /data/.clock 에 남긴다. 다음 부팅에 네트워크가 없어도
 * -r 로 그 시각부터 시작할 수 있다. 1970년보다는 훨씬 낫다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* NTP 시각은 1900-01-01 기준, 유닉스 시각은 1970-01-01 기준.
 * 그 사이 70년(윤년 17번 포함)의 초. */
#define NTP_UNIX_DELTA  2208988800LL

#define NTP_PORT        123
#define DNS_PORT        53
#define CLOCK_FILE      "/data/.clock"

/* 2020-01-01. 이보다 이른 답은 무언가 잘못된 것으로 본다. */
#define SANITY_MIN      1577836800LL
/* 2100-01-01 */
#define SANITY_MAX      4102444800LL

static const char *DEFAULT_SERVERS[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
    "time.google.com",
    NULL
};

/* ── 수신 제한시간 ──────────────────────────────────────────────
 * 응답이 없는 서버에 걸려 부팅이 멈추면 안 된다. */
static void set_timeout(int fd, long seconds)
{
    /* struct __kernel_sock_timeval { s64 tv_sec; s64 tv_usec; } */
    s64 tv[2] = { seconds, 0 };
    lp_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));
}

/* ── DNS ────────────────────────────────────────────────────────
 * A 레코드 하나만 필요하다. 최소한의 질의/응답 처리만 한다. */

/* /etc/resolv.conf 에서 첫 네임서버를 읽는다. 못 읽으면 0. */
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

    /* "nameserver 1.2.3.4" 줄을 찾는다 */
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
            return addr;        /* 네트워크 바이트 순서 */
    }
    return 0;
}

/* 이름을 DNS 질의 형식으로 바꾼다: "a.b.com" -> 1'a' 1'b' 3'c''o''m' 0
 * 반환: 쓴 바이트 수, 넘치면 0. */
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

/* 이름 -> IPv4. 성공하면 네트워크 바이트 순서 주소, 실패하면 0. */
static u32 resolve(const char *host)
{
    /* 이미 숫자 주소면 그대로 쓴다 */
    u32 direct = 0;
    if (ipv4_parse(host, &direct))
        return direct;

    u32 ns = read_nameserver();
    if (ns == 0) {
        dprintf(STDERR_FILENO, "ntp: 네임서버를 모릅니다 "
                "(/etc/resolv.conf 가 비어 있습니다 - dhcp 를 먼저)\n");
        return 0;
    }

    u8     query[512];
    size_t qlen = 0;

    /* 헤더 12바이트: ID, 플래그(재귀 요청), 질문 1개 */
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
    set_timeout((int)fd, 3);

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port   = htons(DNS_PORT);
    to.sin_addr   = ns;

    u32 result = 0;
    if (lp_sendto((int)fd, query, qlen, 0, &to, sizeof(to)) > 0) {
        u8   resp[512];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);

        /* 헤더 + 질문부를 건너뛰고 답을 훑는다. 이름 압축 포인터(0xC0)를
         * 만나면 2바이트, 아니면 라벨 길이만큼 건너뛴다. */
        if (n > 12 && resp[0] == (u8)(id >> 8) && resp[1] == (u8)id) {
            u16 ancount = (u16)((resp[6] << 8) | resp[7]);
            long off = 12;

            /* 질문부의 이름 */
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
                    memcpy(&result, resp + off, 4);   /* 이미 네트워크 순서 */
                    break;
                }
                off += rdlen;
            }
        }
    }

    lp_close((int)fd);
    return result;
}

/* ── SNTP ───────────────────────────────────────────────────────
 * 성공하면 유닉스 초, 실패하면 0. */
static s64 query_ntp(const char *server)
{
    u32 addr = resolve(server);
    if (addr == 0) {
        /* 이름을 못 찾은 것과 서버가 답을 안 하는 것은 원인이 전혀
         * 다르다. 구분해서 알려야 사용자가 어디를 볼지 안다. */
        dprintf(STDERR_FILENO, "ntp: %s: 이름을 찾을 수 없습니다\n", server);
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

    /* 48바이트. 첫 바이트만 채우면 된다:
     *   LI=0(경고 없음) VN=4(NTPv4) Mode=3(클라이언트)
     *   0<<6 | 4<<3 | 3 = 0x23 */
    u8 pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;

    s64 result = 0;
    if (lp_sendto((int)fd, pkt, sizeof(pkt), 0, &to, sizeof(to)) > 0) {
        u8   resp[48];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);
        if (n < 0) {
            char ip[16];
            ipv4_format(addr, ip);
            dprintf(STDERR_FILENO, "ntp: %s (%s): 응답 없음\n", server, ip);
        }
        if (n == (long)sizeof(resp)) {
            /* transmit timestamp: 오프셋 40, 상위 4바이트가 초(빅엔디언) */
            u32 secs = ((u32)resp[40] << 24) | ((u32)resp[41] << 16) |
                       ((u32)resp[42] << 8)  |  (u32)resp[43];
            if (secs != 0)
                result = (s64)secs - NTP_UNIX_DELTA;
        }
    }

    lp_close((int)fd);
    return result;
}

/* ── 저장/복원 ──────────────────────────────────────────────────
 * RTC 가 없으니 우리가 대신한다. 완벽하지 않지만 1970년보다는 낫다. */

static void save_clock(s64 t)
{
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);
    long fd = lp_open(CLOCK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;                      /* /data 가 없으면 그냥 넘어간다 */
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

    /* 우리 libc 에는 atoll 이 없다. 유닉스 초는 이미 10자리라
     * atoi(32비트)로는 부족하므로 여기서 직접 읽는다. */
    s64 v = 0;
    for (const char *c = buf; *c >= '0' && *c <= '9'; c++)
        v = v * 10 + (*c - '0');
    return v;
}

static void report(s64 t)
{
    /* 우리 printf 에는 시각 서식이 없다. 날짜를 직접 계산한다. */
    s64 days = t / 86400;
    int hh = (int)((t % 86400) / 3600);
    int mm = (int)((t % 3600) / 60);
    int ss = (int)(t % 60);

    /* 1970-01-01 부터 날짜 수를 년/월/일로. */
    int y = 1970;
    for (;;) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        int  len  = leap ? 366 : 365;
        if (days < len) break;
        days -= len;
        y++;
    }
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    int mdays[12] = { 31, leap ? 29 : 28, 31, 30, 31, 30,
                      31, 31, 30, 31, 30, 31 };
    int mo = 0;
    while (mo < 12 && days >= mdays[mo]) { days -= mdays[mo]; mo++; }

    printf("ntp: %d-%02d-%02d %02d:%02d:%02d UTC\n",
           y, mo + 1, (int)days + 1, hh, mm, ss);
}

int main(int argc, char **argv)
{
    /* -r: 네트워크 없이 저장해둔 시각으로. 부팅 초반에 쓴다. */
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        s64 saved = load_clock();
        if (saved < SANITY_MIN) {
            dprintf(STDERR_FILENO, "ntp: 저장된 시각이 없습니다\n");
            return 1;
        }
        /* 이미 더 나중이면 되돌리지 않는다. 시계는 뒤로 가면 안 된다. */
        if (lp_time() >= saved) {
            printf("ntp: 시계가 이미 더 앞서 있습니다\n");
            return 0;
        }
        if (lp_settime(saved) < 0) {
            dprintf(STDERR_FILENO, "ntp: 시계를 맞출 수 없습니다 (root 인가요?)\n");
            return 1;
        }
        printf("ntp: 저장된 시각으로 되돌림 (네트워크 아님)\n");
        report(saved);
        return 0;
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

    for (int i = 0; servers[i]; i++) {
        s64 t = query_ntp(servers[i]);
        if (t < SANITY_MIN || t > SANITY_MAX) {
            if (t != 0)
                dprintf(STDERR_FILENO, "ntp: %s 의 답이 이상합니다\n", servers[i]);
            continue;
        }
        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO, "ntp: 시계를 맞출 수 없습니다 (root 인가요?)\n");
            return 1;
        }
        printf("ntp: %s 에서 시각을 받았습니다\n", servers[i]);
        report(t);
        save_clock(t);
        return 0;
    }

    dprintf(STDERR_FILENO,
            "ntp: 시각을 받지 못했습니다.\n"
            "     UDP 123 이 막혀 있거나(공용 와이파이·회사망에서 흔합니다)\n"
            "     아직 주소를 못 받았을 수 있습니다. 다른 서버를 쓰려면:\n"
            "       ntp <서버주소>\n");
    return 1;
}
