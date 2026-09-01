/* unistd.c - 시스템콜 위에 얹은 얇은 래퍼. */
#include "unistd.h"
#include "syscall.h"
#include "string.h"

char **environ = NULL;

/* ── 파일 ─────────────────────────────────────────────────────── */

long lp_open(const char *path, int flags, mode_t mode)
{
    /* AArch64 에는 open 이 없다. openat 에 AT_FDCWD 를 준다. */
    return sys_call4(SYS_openat, AT_FDCWD, (long)path, flags, (long)mode);
}

long lp_close(int fd)                    { return sys_call1(SYS_close, fd); }
long lp_read(int fd, void *b, size_t n)  { return sys_call3(SYS_read, fd, (long)b, (long)n); }
long lp_write(int fd, const void *b, size_t n) { return sys_call3(SYS_write, fd, (long)b, (long)n); }
long lp_lseek(int fd, off_t o, int w)    { return sys_call3(SYS_lseek, fd, o, w); }

long lp_dup(int fd) { return sys_call1(SYS_dup, fd); }

long lp_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return newfd;                    /* dup3 는 같으면 EINVAL 이다 */
    return sys_call3(SYS_dup3, oldfd, newfd, 0);
}

long lp_pipe(int fds[2])                 { return sys_call2(SYS_pipe2, (long)fds, 0); }
long lp_unlink(const char *p)            { return sys_call3(SYS_unlinkat, AT_FDCWD, (long)p, 0); }
long lp_rmdir(const char *p)             { return sys_call3(SYS_unlinkat, AT_FDCWD, (long)p, AT_REMOVEDIR); }
long lp_mkdir(const char *p, mode_t m)   { return sys_call3(SYS_mkdirat, AT_FDCWD, (long)p, (long)m); }
long lp_chdir(const char *p)             { return sys_call1(SYS_chdir, (long)p); }
long lp_getcwd(char *b, size_t n)        { return sys_call2(SYS_getcwd, (long)b, (long)n); }
long lp_access(const char *p, int mode)  { return sys_call4(SYS_faccessat, AT_FDCWD, (long)p, mode, 0); }
long sys_getdents(int fd, void *buf, size_t size) { return sys_call3(SYS_getdents64, fd, (long)buf, (long)size); }

/* newfstatat 의 struct stat 은 arm64 에서 128바이트다.
 * 전체 정의 대신 필요한 필드(st_mode)만 오프셋으로 읽는다.
 *   offset 16 : st_mode (u32) */
#define STAT_BUF_SIZE   128
#define STAT_MODE_OFF   16
#define S_IFMT          0170000
#define S_IFDIR         0040000

static long stat_mode(const char *path, u32 *mode_out)
{
    u8 buf[STAT_BUF_SIZE];
    long r = sys_call4(SYS_newfstatat, AT_FDCWD, (long)path, (long)buf, 0);
    if (r < 0)
        return r;
    *mode_out = *(u32 *)(buf + STAT_MODE_OFF);
    return 0;
}

bool lp_exists(const char *path)
{
    u32 mode;
    return stat_mode(path, &mode) == 0;
}

bool lp_is_dir(const char *path)
{
    u32 mode;
    if (stat_mode(path, &mode) != 0)
        return false;
    return (mode & S_IFMT) == S_IFDIR;
}

/* struct stat 에서 우리가 쓰는 두 필드의 오프셋:
 *   16  st_mode (u32)
 *   48  st_size (s64)
 * 나머지는 건너뛴다. */
#define STAT_SIZE_OFF       48
#define AT_SYMLINK_NOFOLLOW 0x100

long lp_stat(const char *path, lp_stat_t *out, bool follow_symlink)
{
    u8 buf[STAT_BUF_SIZE];
    long r = sys_call4(SYS_newfstatat, AT_FDCWD, (long)path, (long)buf,
                       follow_symlink ? 0 : AT_SYMLINK_NOFOLLOW);
    if (r < 0)
        return r;
    out->mode = *(u32 *)(buf + STAT_MODE_OFF);
    out->size = *(u64 *)(buf + STAT_SIZE_OFF);
    return 0;
}

long lp_rename(const char *from, const char *to)
{
    return sys_call4(SYS_renameat, AT_FDCWD, (long)from, AT_FDCWD, (long)to);
}

long lp_chmod(const char *path, mode_t mode)
{
    return sys_call4(SYS_fchmodat, AT_FDCWD, (long)path, (long)mode, 0);
}

long lp_symlink(const char *target, const char *linkpath)
{
    return sys_call3(SYS_symlinkat, (long)target, AT_FDCWD, (long)linkpath);
}

long lp_readlink(const char *path, char *buf, size_t n)
{
    return sys_call4(SYS_readlinkat, AT_FDCWD, (long)path, (long)buf, (long)n);
}

/* ── 프로세스 ─────────────────────────────────────────────────── */

pid_t lp_fork(void)
{
    /* AArch64 에는 fork 시스템콜이 없다. clone 의 인자는
     *   clone(flags, stack, parent_tid, tls, child_tid)
     * flags 에 종료 시그널만 주고 나머지를 0 으로 두면 fork 와 같다.
     * stack=0 이면 자식이 부모 스택을 copy-on-write 로 이어받는다. */
    return (pid_t)sys_call5(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

long lp_execve(const char *path, char *const argv[], char *const envp[])
{
    return sys_call3(SYS_execve, (long)path, (long)argv, (long)envp);
}

pid_t lp_waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)sys_call4(SYS_wait4, pid, (long)status, options, 0);
}

pid_t lp_wait(int *status)
{
    return lp_waitpid(-1, status, 0);
}

pid_t lp_getpid(void)      { return (pid_t)sys_call0(SYS_getpid); }
long  lp_setsid(void)      { return sys_call0(SYS_setsid); }
long  lp_kill(pid_t p, int s) { return sys_call2(SYS_kill, p, s); }

void lp_exit(int code)
{
    sys_call1(SYS_exit_group, code);
    __builtin_unreachable();
}

long lp_sleep_ms(long ms)
{
    /* struct timespec { long tv_sec; long tv_nsec; } */
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000L };
    return sys_call2(SYS_nanosleep, (long)ts, 0);
}

/* ── 시스템 ───────────────────────────────────────────────────── */

long lp_mount(const char *src, const char *tgt, const char *fstype,
              unsigned long flags, const void *data)
{
    return sys_call5(SYS_mount, (long)src, (long)tgt, (long)fstype,
                     (long)flags, (long)data);
}

long lp_reboot(int cmd)
{
    return sys_call4(SYS_reboot, (long)LINUX_REBOOT_MAGIC1,
                     (long)LINUX_REBOOT_MAGIC2, cmd, 0);
}

/* ── 시각 ─────────────────────────────────────────────────────── */

/* struct timespec 은 arm64 에서 { s64 tv_sec; s64 tv_nsec; } 16바이트다.
 * 구조체를 정의하는 대신 배열로 다룬다. */
#define CLOCK_REALTIME 0

s64 lp_time(void)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, CLOCK_REALTIME, (long)ts) < 0)
        return 0;
    return ts[0];
}

long lp_settime(s64 unix_seconds)
{
    s64 ts[2] = { unix_seconds, 0 };
    return sys_call2(SYS_clock_settime, CLOCK_REALTIME, (long)ts);
}

long lp_sync(void)            { return sys_call0(SYS_sync); }

long lp_swapon(const char *path, int flags)
{
    return sys_call2(SYS_swapon, (long)path, flags);
}

long lp_swapoff(const char *path)
{
    return sys_call1(SYS_swapoff, (long)path);
}

/* /proc 파일은 크기가 0 으로 보고되므로 stat 으로 미리 알 수 없다.
 * 그냥 버퍼가 찰 때까지 읽는다. */
long proc_read(const char *path, char *buf, size_t size)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    size_t total = 0;
    while (total < size - 1) {
        long n = lp_read((int)fd, buf + total, size - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    lp_close((int)fd);

    buf[total] = '\0';
    return (long)total;
}

/* "MemAvailable:  483252 kB" 같은 줄에서 숫자를 뽑는다. */
long proc_find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = text; *p; ) {
        /* 줄 시작에서만 비교한다. 부분 문자열 오탐을 막기 위해서다
         * (예: "SwapFree" 를 찾는데 "MemFree" 에 걸리지 않도록) */
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') p++;

            long v = 0;
            if (*p < '0' || *p > '9')
                return -1;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (*p++ - '0');
            return v;
        }
        /* 다음 줄로 */
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}
long lp_uname(void *buf)      { return sys_call1(SYS_uname, (long)buf); }
long lp_getrandom(void *buf, size_t n, unsigned flags)
{
    return sys_call3(SYS_getrandom, (long)buf, (long)n, (long)flags);
}
