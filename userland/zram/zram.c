/* zram - RAM 안에 압축 스왑을 만든다.
 *
 * 왜 SD 스왑이 아니라 zram 인가:
 *   Zero 2 W 의 SD 는 25MB/s 다. RAM 보다 400배쯤 느리다. SD 로 스왑하면
 *   페이지를 넣었다 뺐다 하느라 시스템이 멈춘 것처럼 되고(thrashing),
 *   SD 는 쓰기 횟수 제한이 있어 수명도 급격히 깎인다.
 *   zram 은 스왑 대상을 RAM 안에 두고 압축해서 보관한다. 보통 3배쯤
 *   압축되므로 실질 가용 메모리가 늘어나고 SD 에는 한 바이트도 쓰지 않는다.
 *   안드로이드와 크롬OS 가 저사양 기기에서 쓰는 방식이다.
 *
 *   zram 이 RAM 을 쓰긴 하므로 공짜는 아니다. 압축이 잘 되는 데이터일 때만
 *   이득이다. 그래도 SD 스왑보다는 모든 면에서 낫다.
 *
 * 사용법:
 *   zram on [크기MB]    기본 256MB
 *   zram off
 *   zram status
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define ZRAM_DEV      "/dev/zram0"
#define ZRAM_SYS      "/sys/block/zram0"
#define DEFAULT_MB    256
#define PAGE_SIZE     4096

/* 스왑 헤더 v1_2 배치 (첫 페이지 안).
 *   0..1023      부트 영역 (건드리지 않는다)
 *   1024         version      u32 = 1
 *   1028         last_page    u32 = 페이지수 - 1
 *   1032         nr_badpages  u32 = 0
 *   PAGE-10      "SWAPSPACE2" 매직
 */
#define SWAP_VERSION_OFF    1024
#define SWAP_LASTPAGE_OFF   1028
#define SWAP_BADPAGES_OFF   1032
#define SWAP_MAGIC_OFF      (PAGE_SIZE - 10)
#define SWAP_MAGIC          "SWAPSPACE2"

static long sysfs_write(const char *path, const char *val)
{
    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0)
        return fd;

    long rc = lp_write((int)fd, val, strlen(val));
    lp_close((int)fd);
    return rc;
}

static long sysfs_read_num(const char *path)
{
    char buf[64];
    long n = proc_read(path, buf, sizeof(buf));
    if (n < 0)
        return n;

    long v = 0;
    for (const char *p = buf; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    return v;
}

/* 첫 페이지에 스왑 헤더를 쓴다 (mkswap 이 하는 일). */
static int write_swap_header(const char *dev, u64 bytes)
{
    long fd = lp_open(dev, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "zram: %s 를 열 수 없습니다 (%ld)\n", dev, -fd);
        return 1;
    }

    static u8 page[PAGE_SIZE];
    memset(page, 0, sizeof(page));

    u32 nr_pages = (u32)(bytes / PAGE_SIZE);
    if (nr_pages < 2) {
        dprintf(STDERR_FILENO, "zram: 크기가 너무 작습니다\n");
        lp_close((int)fd);
        return 1;
    }

    *(u32 *)(page + SWAP_VERSION_OFF)   = 1;
    *(u32 *)(page + SWAP_LASTPAGE_OFF)  = nr_pages - 1;
    *(u32 *)(page + SWAP_BADPAGES_OFF)  = 0;
    memcpy(page + SWAP_MAGIC_OFF, SWAP_MAGIC, 10);

    long n = lp_write((int)fd, page, sizeof(page));
    lp_close((int)fd);

    if (n != (long)sizeof(page)) {
        dprintf(STDERR_FILENO, "zram: 헤더 쓰기 실패 (%ld)\n", n);
        return 1;
    }
    return 0;
}

static int cmd_on(u64 mb)
{
    /* 1) 크기 지정. sysfs 가 없으면 커널에 CONFIG_ZRAM 이 없는 것이다. */
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%luM", (unsigned long)mb);

    long rc = sysfs_write(ZRAM_SYS "/disksize", size_str);
    if (rc < 0) {
        dprintf(STDERR_FILENO,
                "zram: %s/disksize 에 쓸 수 없습니다 (%ld)\n"
                "      커널에 CONFIG_ZRAM 이 있습니까?\n", ZRAM_SYS, -rc);
        return 1;
    }

    /* 2) 스왑 헤더 */
    if (write_swap_header(ZRAM_DEV, mb * 1024 * 1024) != 0)
        return 1;

    /* 3) 스왑으로 올린다. 우선순위를 높여 다른 스왑보다 먼저 쓰게 한다. */
    rc = lp_swapon(ZRAM_DEV, 0);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "zram: swapon 실패 (%ld)\n", -rc);
        return 1;
    }

    printf("zram: %luMB 압축 스왑을 RAM 에 만들었습니다\n", (unsigned long)mb);
    return 0;
}

static int cmd_off(void)
{
    long rc = lp_swapoff(ZRAM_DEV);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "zram: swapoff 실패 (%ld)\n", -rc);
        return 1;
    }
    sysfs_write(ZRAM_SYS "/reset", "1");
    printf("zram: 해제했습니다\n");
    return 0;
}

static int cmd_status(void)
{
    long disksize = sysfs_read_num(ZRAM_SYS "/disksize");
    if (disksize <= 0) {
        printf("zram: 설정되지 않았습니다\n");
        return 0;
    }

    char mem[4096];
    long swap_total = -1, swap_free = -1;
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
        swap_total = proc_find_kv(mem, "SwapTotal");
        swap_free  = proc_find_kv(mem, "SwapFree");
    }

    printf("zram 장치 크기 : %ld MB\n", disksize / 1024 / 1024);
    if (swap_total >= 0)
        printf("스왑 전체      : %ld MB\n", swap_total / 1024);
    if (swap_total > 0 && swap_free >= 0)
        printf("스왑 사용      : %ld MB\n", (swap_total - swap_free) / 1024);

    /* 압축 효과. orig_data_size 는 압축 전, compr_data_size 는 압축 후. */
    long orig  = sysfs_read_num(ZRAM_SYS "/orig_data_size");
    long compr = sysfs_read_num(ZRAM_SYS "/compr_data_size");
    if (orig > 0 && compr > 0)
        printf("압축률         : %ld%% (%ld KB -> %ld KB)\n",
               compr * 100 / orig, orig / 1024, compr / 1024);

    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("사용법: zram on [크기MB] | zram off | zram status\n");
        return 2;
    }

    if (strcmp(argv[1], "on") == 0) {
        u64 mb = DEFAULT_MB;
        if (argc > 2) {
            long v = strtol(argv[2], NULL, 10);
            if (v <= 0) {
                dprintf(STDERR_FILENO, "zram: 크기가 잘못되었습니다\n");
                return 2;
            }
            mb = (u64)v;
        }
        return cmd_on(mb);
    }
    if (strcmp(argv[1], "off") == 0)     return cmd_off();
    if (strcmp(argv[1], "status") == 0)  return cmd_status();

    dprintf(STDERR_FILENO, "zram: 알 수 없는 명령 '%s'\n", argv[1]);
    return 2;
}
