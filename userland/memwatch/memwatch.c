/* memwatch - 메모리 안전장치.
 *
 * 리눅스에는 이미 OOM Killer 가 있다. 그런데 그것만 믿기 어려운 이유가 둘 있다:
 *   1) 너무 늦게 동작한다. 커널이 정말 할당에 실패하는 지점까지 가면
 *      그 전에 이미 시스템이 한참 버벅인다.
 *   2) 무엇을 죽일지 우리가 정하지 못한다. 셸이나 SSH 서버가 먼저
 *      죽으면 원격에서 손을 쓸 수가 없다.
 *
 * 그래서 그보다 먼저, 우리가 정한 기준으로 개입한다:
 *   여유 < WARN_MB   -> 콘솔에 경고
 *   여유 < RESERVE_MB -> 보호 대상이 아닌 프로세스 중 가장 큰 것을 종료
 *
 * 보호 대상(기본): init, memwatch, sh, dropbear, wpa_supplicant
 * 이들이 살아 있어야 상황을 보고 손을 쓸 수 있다.
 *
 * 두 겹으로 막는다:
 *   1) 우리가 먼저 개입한다 (여유 32MB 지점)
 *   2) 그래도 커널 OOM Killer 까지 가면 - 커널은 우리 보호 목록을
 *      모른다. 그래서 각 프로세스의 /proc/<pid>/oom_score_adj 를
 *      직접 조정해 커널에게 알려준다:
 *        -1000  절대 죽이지 않는다 (보호 대상)
 *          500  먼저 죽는다 (그 외)
 *      이 값은 fork 로 상속되므로, dropbear 가 접속마다 만드는 자식
 *      프로세스도 자동으로 보호된다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define RESERVE_MB    32      /* 시스템 필수 여유. 이 아래로 가면 정리한다 */
#define WARN_MB       64      /* 경고를 시작하는 지점 */
#define POLL_MS      1000     /* 평상시 확인 주기 */
#define POLL_BUSY_MS  200     /* 압박 상황에서의 주기 */
#define MAX_PROCS     256
#define TERM_GRACE_MS 500     /* SIGTERM 후 SIGKILL 까지 기다리는 시간 */

/* /proc/<pid>/oom_score_adj 값.
 * -1000 이면 커널 OOM Killer 가 그 프로세스를 후보에서 아예 제외한다. */
#define OOM_PROTECT   (-1000)
#define OOM_SACRIFICE    500

typedef struct {
    pid_t pid;
    long  rss_kb;
    char  name[32];
} proc_t;

/* 이들은 죽이지 않는다. 죽으면 상황 파악도 복구도 못 한다. */
static const char *PROTECTED[] = {
    "init", "memwatch", "sh", "dropbear", "wpa_supplicant", NULL
};

static bool is_protected(const char *name, pid_t pid, pid_t self)
{
    if (pid == 1 || pid == self)
        return true;
    for (int i = 0; PROTECTED[i]; i++)
        if (strcmp(name, PROTECTED[i]) == 0)
            return true;
    return false;
}

/* 커널에게 이 프로세스의 우선순위를 알린다.
 * 이미 원하는 값이면 쓰지 않는다 - 매 초 불필요하게 쓰지 않기 위해서다. */
static void set_oom_score(pid_t pid, int score)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)pid);

    char cur[32];
    if (proc_read(path, cur, sizeof(cur)) > 0) {
        long v = strtol(cur, NULL, 10);
        if (v == score)
            return;                 /* 이미 맞다 */
    }

    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0)
        return;                     /* 그 사이 종료된 프로세스 */

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", score);
    lp_write((int)fd, buf, (size_t)n);
    lp_close((int)fd);
}

/* /proc/<pid>/comm 에서 프로세스 이름을 읽는다 (개행 제거) */
static bool read_comm(pid_t pid, char *out, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    char buf[64];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return false;

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, size);
    return true;
}

/* /proc/<pid>/statm 의 두 번째 값이 상주 페이지 수다.
 *   size resident shared text lib data dt   (모두 페이지 단위) */
static long read_rss_kb(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);

    char buf[128];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return -1;

    char *p = buf;
    while (*p && *p != ' ') p++;      /* 첫 번째 값 건너뛰기 */
    while (*p == ' ') p++;

    long pages = strtol(p, NULL, 10);
    if (pages <= 0)
        return -1;

    return pages * 4;                  /* 4KB 페이지 -> KB */
}

/* /proc 를 훑어 프로세스 목록을 만든다. 반환: 개수 */
static int scan_processes(proc_t *list, int max, pid_t self)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    static char buf[8192];
    int n = 0;

    for (;;) {
        long got = sys_getdents(fd, buf, sizeof(buf));
        if (got <= 0)
            break;

        for (long off = 0; off < got && n < max; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + 16);
            char *name = rec + 19;
            if (len == 0) break;
            off += len;

            /* 숫자로만 된 이름이 프로세스 디렉터리다 */
            if (name[0] < '1' || name[0] > '9')
                continue;
            bool numeric = true;
            for (char *c = name; *c; c++)
                if (*c < '0' || *c > '9') { numeric = false; break; }
            if (!numeric)
                continue;

            pid_t pid = (pid_t)strtol(name, NULL, 10);
            long rss = read_rss_kb(pid);
            if (rss < 0)
                continue;               /* 그 사이 종료된 프로세스 */

            list[n].pid    = pid;
            list[n].rss_kb = rss;
            if (!read_comm(pid, list[n].name, sizeof(list[n].name)))
                strlcpy(list[n].name, "?", sizeof(list[n].name));
            (void)self;
            n++;
        }
    }

    lp_close((int)fd);
    return n;
}

/* 스캔한 프로세스들에 커널 우선순위를 적용한다.
 *
 * 새 프로세스가 계속 생기므로(특히 dropbear 는 접속마다 자식을 만든다)
 * 한 번만 설정하는 것으로는 부족하다. 주기적으로 다시 확인한다. */
static void apply_oom_policy(const proc_t *list, int n, pid_t self)
{
    for (int i = 0; i < n; i++) {
        bool prot = is_protected(list[i].name, list[i].pid, self);
        set_oom_score(list[i].pid, prot ? OOM_PROTECT : OOM_SACRIFICE);
    }
}

/* 보호 대상이 아닌 것 중 가장 큰 프로세스를 종료한다.
 * 반환: 종료했으면 true */
static bool kill_largest(pid_t self, long need_kb)
{
    static proc_t list[MAX_PROCS];
    int n = scan_processes(list, MAX_PROCS, self);

    int  best = -1;
    long best_rss = 0;

    for (int i = 0; i < n; i++) {
        if (is_protected(list[i].name, list[i].pid, self))
            continue;
        if (list[i].rss_kb > best_rss) {
            best_rss = list[i].rss_kb;
            best = i;
        }
    }

    if (best < 0) {
        dprintf(STDERR_FILENO,
                "memwatch: 정리할 수 있는 프로세스가 없습니다."
                " 보호 대상만 남았습니다 (%ldKB 부족)\n", need_kb);
        return false;
    }

    dprintf(STDERR_FILENO,
            "memwatch: 메모리 부족 - %s (pid %d, %ldKB) 를 종료합니다\n",
            list[best].name, (int)list[best].pid, list[best].rss_kb);

    /* 먼저 정리할 기회를 준다 */
    lp_kill(list[best].pid, SIGTERM);
    lp_sleep_ms(TERM_GRACE_MS);

    /* 아직 살아 있으면 강제 종료. kill(pid, 0) 은 존재 확인용이다. */
    if (lp_kill(list[best].pid, 0) == 0) {
        lp_kill(list[best].pid, SIGKILL);
        dprintf(STDERR_FILENO, "memwatch:   응답이 없어 강제 종료했습니다\n");
    }
    return true;
}

int main(int argc, char **argv)
{
    long reserve_kb = RESERVE_MB * 1024;
    long warn_kb    = WARN_MB * 1024;
    bool daemonize  = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            reserve_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            warn_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else {
            printf("사용법: memwatch [-d] [-r 예약MB] [-w 경고MB]\n");
            printf("  -d  백그라운드로\n");
            printf("  -r  시스템 필수 여유 (기본 %d MB)\n", RESERVE_MB);
            printf("  -w  경고 시작 지점 (기본 %d MB)\n", WARN_MB);
            return 2;
        }
    }

    if (warn_kb < reserve_kb)
        warn_kb = reserve_kb;

    if (daemonize) {
        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "memwatch: fork 실패\n");
            return 1;
        }
        if (pid > 0)
            return 0;               /* 부모는 즉시 빠진다 */
        lp_setsid();
    }

    char mem[4096];
    long total = -1;
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0)
        total = proc_find_kv(mem, "MemTotal");

    if (total < 0) {
        dprintf(STDERR_FILENO,
                "memwatch: /proc/meminfo 를 읽을 수 없습니다"
                " (/proc 가 마운트되어 있습니까?)\n");
        return 1;
    }

    printf("memwatch: 전체 %ldMB, 예약 %ldMB, 경고 %ldMB 에서 시작\n",
           total / 1024, reserve_kb / 1024, warn_kb / 1024);

    pid_t self = lp_getpid();
    bool  warned = false;
    int   tick = 0;

    /* 시작하자마자 한 번 적용한다. 이게 있어야 memwatch 가 개입하기 전에
     * 커널 OOM Killer 가 먼저 돌더라도 SSH 와 셸이 살아남는다. */
    {
        static proc_t boot_list[MAX_PROCS];
        int n = scan_processes(boot_list, MAX_PROCS, self);
        apply_oom_policy(boot_list, n, self);
        printf("memwatch: 커널 우선순위 적용 (%d개 프로세스)\n", n);
        printf("memwatch:   보호 = init, memwatch, sh, dropbear, wpa_supplicant\n");
    }

    for (;;) {
        long avail = -1, swap_total = -1, swap_free = -1;

        if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
            avail      = proc_find_kv(mem, "MemAvailable");
            swap_total = proc_find_kv(mem, "SwapTotal");
            swap_free  = proc_find_kv(mem, "SwapFree");
        }

        if (avail < 0) {
            lp_sleep_ms(POLL_MS);
            continue;
        }

        /* 5초마다 새로 생긴 프로세스에도 우선순위를 적용한다.
         * dropbear 는 접속마다 자식을 만들고, 셸은 명령마다 만든다. */
        if (++tick >= 5) {
            tick = 0;
            static proc_t all[MAX_PROCS];
            int n = scan_processes(all, MAX_PROCS, self);
            apply_oom_policy(all, n, self);
        }

        if (avail < reserve_kb) {
            long swap_used = (swap_total > 0 && swap_free >= 0)
                             ? swap_total - swap_free : 0;
            dprintf(STDERR_FILENO,
                    "\nmemwatch: ★ 메모리 한계 - 여유 %ldMB (예약 %ldMB)"
                    "%s\n", avail / 1024, reserve_kb / 1024,
                    swap_used > 0 ? ", 스왑 사용 중" : "");

            /* 한 번에 하나씩 정리하고 다시 확인한다.
             * 한꺼번에 여러 개를 죽이면 필요 이상으로 정리된다. */
            kill_largest(self, reserve_kb - avail);
            warned = true;
            lp_sleep_ms(POLL_BUSY_MS);
            continue;
        }

        if (avail < warn_kb) {
            if (!warned) {
                dprintf(STDERR_FILENO,
                        "memwatch: 경고 - 여유 메모리 %ldMB\n", avail / 1024);
                warned = true;
            }
            lp_sleep_ms(POLL_BUSY_MS);
            continue;
        }

        /* 여유를 회복하면 다음 경고를 다시 낼 수 있게 초기화한다 */
        if (warned) {
            dprintf(STDERR_FILENO,
                    "memwatch: 회복 - 여유 메모리 %ldMB\n", avail / 1024);
            warned = false;
        }
        lp_sleep_ms(POLL_MS);
    }
}
