/* sysinfo - 시스템 상태를 한눈에.
 *
 * 커널이 /proc 과 /sys 에 이미 다 내놓았다. 우리가 할 일은 흩어진 값을
 * 모아 읽기 좋게 내놓는 것뿐이다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"
#include "net.h"

/* struct utsname - 각 필드 65바이트, 총 390 */
#define UTS_LEN       65
#define UTS_SYSNAME    0
#define UTS_NODENAME  65
#define UTS_RELEASE  130
#define UTS_VERSION  195
#define UTS_MACHINE  260
#define UTS_SIZE     390

/* struct statfs (arm64) 오프셋 */
#define STATFS_SIZE    120
#define STATFS_BSIZE     8
#define STATFS_BLOCKS   16
#define STATFS_BAVAIL   32

static void hr(void) { printf("─────────────────────────────────────────────\n"); }

/* /proc/cpuinfo 에서 첫 번째로 나오는 키의 값을 가져온다.
 * 형식이 "key : value" 라 meminfo 파서를 쓸 수 없다. */
static bool cpuinfo_field(const char *text, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    for (const char *p = text; *p; ) {
        if (strncmp(p, key, klen) == 0) {
            const char *c = strchr(p, ':');
            const char *e = strchr(p, '\n');
            if (c && (!e || c < e)) {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                size_t len = e ? (size_t)(e - c) : strlen(c);
                if (len >= n) len = n - 1;
                memcpy(out, c, len);
                out[len] = '\0';
                return true;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return false;
}

static int count_cpus(const char *cpuinfo)
{
    int n = 0;
    for (const char *p = cpuinfo; *p; ) {
        if (strncmp(p, "processor", 9) == 0) n++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return n;
}

static void show_system(void)
{
    u8 uts[UTS_SIZE];
    memset(uts, 0, sizeof(uts));

    printf("\n[시스템]\n");
    if (lp_uname(uts) == 0) {
        printf("  커널       %s %s\n", (char *)uts + UTS_SYSNAME,
               (char *)uts + UTS_RELEASE);
        printf("  아키텍처   %s\n", (char *)uts + UTS_MACHINE);
        printf("  호스트명   %s\n", (char *)uts + UTS_NODENAME);
    }

    char buf[256];
    if (proc_read("/proc/uptime", buf, sizeof(buf)) > 0) {
        long secs = strtol(buf, NULL, 10);
        printf("  가동 시간  %ld일 %ld시간 %ld분 %ld초\n",
               secs / 86400, (secs % 86400) / 3600,
               (secs % 3600) / 60, secs % 60);
    }
    if (proc_read("/proc/loadavg", buf, sizeof(buf)) > 0) {
        char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
        printf("  부하       %s\n", buf);
    }
}

static void show_cpu(void)
{
    static char cpuinfo[8192];
    if (proc_read("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo)) <= 0)
        return;

    printf("\n[CPU]\n");
    printf("  코어 수    %d\n", count_cpus(cpuinfo));

    char v[128];
    if (cpuinfo_field(cpuinfo, "CPU implementer", v, sizeof(v)))
        printf("  제조사     %s%s\n", v,
               strcmp(v, "0x41") == 0 ? "  (ARM)" : "");
    if (cpuinfo_field(cpuinfo, "CPU part", v, sizeof(v)))
        printf("  파트       %s%s\n", v,
               strcmp(v, "0xd03") == 0 ? "  (Cortex-A53)" : "");
    if (cpuinfo_field(cpuinfo, "Features", v, sizeof(v)))
        printf("  기능       %s\n", v);

    /* SoC 온도. 라즈베리파이는 thermal_zone0 에 밀리섭씨로 내놓는다. */
    char t[32];
    if (proc_read("/sys/class/thermal/thermal_zone0/temp", t, sizeof(t)) > 0) {
        long mc = strtol(t, NULL, 10);
        printf("  온도       %ld.%ld C\n", mc / 1000, (mc % 1000) / 100);
    }
}

static void show_memory(void)
{
    static char mem[8192];
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) <= 0)
        return;

    long total = proc_find_kv(mem, "MemTotal");
    long avail = proc_find_kv(mem, "MemAvailable");
    long swt   = proc_find_kv(mem, "SwapTotal");
    long swf   = proc_find_kv(mem, "SwapFree");

    printf("\n[메모리]\n");
    if (total > 0) {
        long used = total - (avail > 0 ? avail : 0);
        printf("  전체       %4ld MB\n", total / 1024);
        printf("  사용       %4ld MB  (%ld%%)\n", used / 1024, used * 100 / total);
        printf("  여유       %4ld MB\n", avail / 1024);
    }
    if (swt > 0) {
        printf("  zram 스왑  %4ld MB 중 %ld MB 사용\n",
               swt / 1024, (swt - swf) / 1024);
        /* 압축 효과 */
        char n1[32], n2[32];
        if (proc_read("/sys/block/zram0/orig_data_size", n1, sizeof(n1)) > 0 &&
            proc_read("/sys/block/zram0/compr_data_size", n2, sizeof(n2)) > 0) {
            long orig = strtol(n1, NULL, 10), compr = strtol(n2, NULL, 10);
            if (orig > 0 && compr > 0)
                printf("  압축률     %ld%%  (%ld KB -> %ld KB)\n",
                       compr * 100 / orig, orig / 1024, compr / 1024);
        }
    } else {
        printf("  zram 스왑  없음\n");
    }
}

static void show_one_fs(const char *path, const char *label)
{
    u8 st[STATFS_SIZE];
    memset(st, 0, sizeof(st));
    if (sys_call2(SYS_statfs, (long)path, (long)st) < 0)
        return;

    u64 bs     = *(u64 *)(st + STATFS_BSIZE);
    u64 blocks = *(u64 *)(st + STATFS_BLOCKS);
    u64 avail  = *(u64 *)(st + STATFS_BAVAIL);
    if (bs == 0 || blocks == 0)
        return;

    u64 total_mb = blocks * bs / 1048576;
    u64 avail_mb = avail  * bs / 1048576;
    u64 used_mb  = total_mb - avail_mb;

    printf("  %-10s %5lu MB 중 %lu MB 사용 (%lu%%), 여유 %lu MB\n",
           label, (unsigned long)total_mb, (unsigned long)used_mb,
           total_mb ? (unsigned long)(used_mb * 100 / total_mb) : 0UL,
           (unsigned long)avail_mb);
}

static void show_storage(void)
{
    printf("\n[저장장치]\n");
    show_one_fs("/",     "/ (RAM)");
    show_one_fs("/data", "/data");
}

static void show_network(void)
{
    static const char *IFACES[] = { "wlan0", "eth0", "usb0", "lo", NULL };

    printf("\n[네트워크]\n");
    bool any = false;

    for (int i = 0; IFACES[i]; i++) {
        u32 addr = 0;
        if (net_get_addr(IFACES[i], &addr) < 0)
            continue;

        char ip[16];
        ipv4_format(addr, ip);

        u8 mac[6];
        bool has_mac = net_if_hwaddr(IFACES[i], mac) == 0;

        printf("  %-6s %-15s %s", IFACES[i], ip,
               net_if_is_up(IFACES[i]) ? "UP" : "DOWN");
        if (has_mac)
            printf("  %02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        printf("\n");
        any = true;
    }

    if (!any)
        printf("  주소가 할당된 인터페이스가 없습니다\n");

    char dns[128];
    if (proc_read("/etc/resolv.conf", dns, sizeof(dns)) > 0) {
        char *p = strstr(dns, "nameserver ");
        if (p) {
            p += 11;
            char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
            printf("  DNS    %s\n", p);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    hr();
    printf("  LP-zero OS  시스템 정보\n");
    hr();

    show_system();
    show_cpu();
    show_memory();
    show_storage();
    show_network();

    printf("\n");
    return 0;
}
