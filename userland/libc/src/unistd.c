/* unistd.c - thin wrappers over the system calls. */
#include "unistd.h"
#include "syscall.h"
#include "string.h"

char **environ = NULL;

/* ── Files ─────────────────────────────────────────────────────── */

long lp_open(const char *path, int flags, mode_t mode)
{
    /* AArch64 has no open. Use openat with AT_FDCWD. */
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
        return newfd;                    /* dup3 returns EINVAL when they match */
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

/* newfstatat's struct stat is 128 bytes on arm64. Rather than declaring
 * the whole thing we read the one field we need (st_mode) by offset.
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

/* Offsets of the two struct stat fields we use:
 *   16  st_mode (u32)
 *   48  st_size (s64)
 * everything else is skipped. */
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

/* ── Processes ─────────────────────────────────────────────────── */

pid_t lp_fork(void)
{
    /* AArch64 has no fork system call. clone's arguments are
     *   clone(flags, stack, parent_tid, tls, child_tid)
     * Passing only the exit signal in flags and zero for the rest is fork.
     * With stack=0 the child inherits the parent's stack copy-on-write. */
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

/* ── System ────────────────────────────────────────────────────── */

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

/* ── The terminal ──────────────────────────────────────────────── */

long lp_ioctl(int fd, unsigned long req, void *arg)
{
    return sys_call3(SYS_ioctl, fd, (long)req, (long)arg);
}

#define TCGETS       0x5401
#define TCSETS       0x5402
#define TIOCGWINSZ   0x5413

/* struct termios layout (arm64):
 *    0  c_iflag (u32)
 *    4  c_oflag (u32)
 *    8  c_cflag (u32)
 *   12  c_lflag (u32)
 *   16  c_line  (u8)
 *   17  c_cc[19]
 */
#define T_IFLAG  0
#define T_OFLAG  1
#define T_LFLAG  3
#define T_CC     17
#define VTIME    5
#define VMIN     6

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define IEXTEN  0x8000
/* c_iflag */
#define BRKINT  0x0002
#define ISTRIP  0x0020
#define INLCR   0x0040
#define ICRNL   0x0100
#define IXON    0x0400
/* IUTF8: the kernel's line erase treats UTF-8 as whole characters */
#define IUTF8   0x4000
/* c_oflag */
#define OPOST   0x0001

long lp_term_raw(int fd, lp_termios_t *saved)
{
    long r = lp_ioctl(fd, TCGETS, saved->raw);
    if (r < 0)
        return r;

    lp_termios_t t = *saved;
    u32 *f = (u32 *)t.raw;

    /* Turn off the kernel's line editing, echo and signals - we draw it
     * all ourselves. IXON has to go or Ctrl-S freezes the screen instead
     * of reaching us. ICRNL has to go so Enter arrives as a plain CR.
     * OPOST has to go so \n is not turned into CRLF - we emit that. */
    f[T_LFLAG] &= ~(u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] &= ~(u32)(IXON | ICRNL | BRKINT | ISTRIP | INLCR);
    f[T_OFLAG] &= ~(u32)OPOST;

    /* Return as soon as one byte arrives (VMIN=1, VTIME=0). */
    f[T_IFLAG] |= IUTF8;        /* keep telling the terminal it is UTF-8 */

    t.raw[T_CC + VMIN]  = 1;
    t.raw[T_CC + VTIME] = 0;

    return lp_ioctl(fd, TCSETS, t.raw);
}

long lp_term_restore(int fd, const lp_termios_t *saved)
{
    return lp_ioctl(fd, TCSETS, (void *)saved->raw);
}

long lp_term_set_utf8(int fd)
{
    lp_termios_t t;
    long r = lp_ioctl(fd, TCGETS, t.raw);
    if (r < 0)
        return r;
    u32 *f = (u32 *)t.raw;
    f[T_IFLAG] |= IUTF8;
    return lp_ioctl(fd, TCSETS, t.raw);
}

long lp_term_size(int fd, int *rows, int *cols)
{
    /* struct winsize { u16 row, col, xpixel, ypixel; } */
    u16 ws[4] = { 0, 0, 0, 0 };
    long r = lp_ioctl(fd, TIOCGWINSZ, ws);
    if (r < 0 || ws[0] == 0 || ws[1] == 0) {
        *rows = 24;
        *cols = 80;
        return -1;
    }
    *rows = ws[0];
    *cols = ws[1];
    return 0;
}

/* ── Time ──────────────────────────────────────────────────────── */

/* struct timespec on arm64 is { s64 tv_sec; s64 tv_nsec; }, 16 bytes.
 * We use an array rather than declaring the struct. */
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

/* Leap years: divisible by 4, except by 100, unless also by 400.
 * So 2000 was a leap year and 1900 was not. */
static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int MDAYS[12] = { 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31 };

static int days_in_month(int y, int m)   /* m: 0-11 */
{
    return (m == 1 && is_leap(y)) ? 29 : MDAYS[m];
}

void lp_gmtime(s64 t, lp_tm_t *out)
{
    /* Negative values (before 1970) have to work too. C division truncates
     * toward zero, which would be a day out, so we floor it instead. */
    s64 days = t / 86400;
    s64 rem  = t % 86400;
    if (rem < 0) { rem += 86400; days--; }

    out->hour = (int)(rem / 3600);
    out->min  = (int)((rem % 3600) / 60);
    out->sec  = (int)(rem % 60);

    /* 1970-01-01 was a Thursday (4). */
    out->wday = (int)((days % 7 + 11) % 7);

    int y = 1970;
    while (days < 0) {
        y--;
        days += is_leap(y) ? 366 : 365;
    }
    for (;;) {
        int len = is_leap(y) ? 366 : 365;
        if (days < len) break;
        days -= len;
        y++;
    }
    out->year = y;

    int m = 0;
    while (m < 11 && days >= days_in_month(y, m)) {
        days -= days_in_month(y, m);
        m++;
    }
    out->mon = m + 1;
    out->day = (int)days + 1;
}

s64 lp_timegm(const lp_tm_t *tm)
{
    s64 days = 0;

    if (tm->year >= 1970) {
        for (int y = 1970; y < tm->year; y++)
            days += is_leap(y) ? 366 : 365;
    } else {
        for (int y = tm->year; y < 1970; y++)
            days -= is_leap(y) ? 366 : 365;
    }
    for (int m = 0; m < tm->mon - 1; m++)
        days += days_in_month(tm->year, m);
    days += tm->day - 1;

    return days * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec;
}

long lp_sync(void)            { return sys_call0(SYS_sync); }

/* ── Scheduling priority ──────────────────────────────────────────────
 * PRIO_PROCESS = 0. The kernel does not hand the nice value back as it
 * went in: getpriority returns 20 - nice, so that a valid result is
 * never negative and cannot be mistaken for an error code. We undo
 * that here, so callers see the nice value they set. */
#define PRIO_PROCESS 0

long lp_setpriority(pid_t pid, int nice_value)
{
    return sys_call3(SYS_setpriority, PRIO_PROCESS, (long)pid,
                     (long)nice_value);
}

int lp_getpriority(pid_t pid)
{
    long rc = sys_call2(SYS_getpriority, PRIO_PROCESS, (long)pid);
    if (rc < 0)
        return 0;
    return (int)(20 - rc);
}

/* ── Filesystem space ─────────────────────────────────────────────────
 * struct statfs64 on arm64, the three fields we need:
 *    8  f_bsize    block size
 *   16  f_blocks   blocks in total
 *   32  f_bavail   blocks an ordinary user may still use
 * f_bfree (24) is larger: it includes the 5% ext4 keeps back for root.
 * f_bavail is the honest number. */
#define STATFS_BUF_SIZE   120
#define STATFS_OFF_BSIZE    8
#define STATFS_OFF_BLOCKS  16
#define STATFS_OFF_BAVAIL  32

long lp_fs_space(const char *path, u64 *free_bytes, u64 *total_bytes)
{
    u8 buf[STATFS_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    long rc = sys_call2(SYS_statfs, (long)path, (long)buf);
    if (rc < 0)
        return rc;

    u64 bsize  = *(u64 *)(buf + STATFS_OFF_BSIZE);
    u64 blocks = *(u64 *)(buf + STATFS_OFF_BLOCKS);
    u64 avail  = *(u64 *)(buf + STATFS_OFF_BAVAIL);

    if (free_bytes)  *free_bytes  = avail * bsize;
    if (total_bytes) *total_bytes = blocks * bsize;
    return 0;
}

long lp_swapon(const char *path, int flags)
{
    return sys_call2(SYS_swapon, (long)path, flags);
}

long lp_swapoff(const char *path)
{
    return sys_call1(SYS_swapoff, (long)path);
}

/* /proc files report a size of 0, so stat tells us nothing in advance.
 * Just read until the buffer is full. */
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

/* Pull the number out of a line like "MemAvailable:  483252 kB". */
long proc_find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = text; *p; ) {
        /* Match only at the start of a line, so a substring cannot fool us
         * (looking for "SwapFree" must not match inside "MemFree"). */
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
        /* on to the next line */
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
