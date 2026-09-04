/* unistd.c - thin wrappers over the system calls. */
#include "unistd.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

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
    out->mode  = *(u32 *)(buf + STAT_MODE_OFF);
    out->size  = *(u64 *)(buf + STAT_SIZE_OFF);
    /* The rest of struct stat on arm64, by offset rather than by
     * declaring the struct - nothing then depends on padding rules.
     *   20 nlink   24 uid   28 gid   88 mtime */
    out->nlink = *(u32 *)(buf + 20);
    out->uid   = *(u32 *)(buf + 24);
    out->gid   = *(u32 *)(buf + 28);
    out->mtime = *(s64 *)(buf + 88);
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
/* A negative pid is passed straight through, because that is how the
 * kernel is told "the whole process group": kill(-pgid, sig). Against a
 * fork bomb that is the difference between one syscall and one per
 * process. */
long  lp_kill(pid_t p, int s) { return sys_call2(SYS_kill, p, s); }

long lp_setrlimit(int resource, u64 soft, u64 hard)
{
    /* struct rlimit64 { u64 rlim_cur; u64 rlim_max; } */
    u64 lim[2] = { soft, hard };
    /* prlimit64(pid=0 meaning ourselves, resource, new, old) */
    return sys_call4(SYS_prlimit64, 0, resource, (long)lim, 0);
}

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
#define VSUSP    10

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
#define ONLCR   0x0004

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

/* Put a terminal back into the state a person expects.
 *
 * A program that dies while it owns the terminal - killed, or crashed -
 * leaves it however it was: no echo, no line editing, newlines that do
 * not return to column one. The shell then looks broken, and the way
 * out is to type a command you cannot see into a terminal that is not
 * listening properly.
 *
 * So the shell puts it back after every command rather than trusting
 * each program to clean up after itself. This sets only the flags that
 * matter for a usable terminal and leaves the rest - baud rate, control
 * characters - as they were. */
bool lp_isatty(int fd)
{
    lp_termios_t t;
    return lp_ioctl(fd, TCGETS, t.raw) == 0;
}

long lp_term_sane(int fd)
{
    lp_termios_t t;
    if (lp_ioctl(fd, TCGETS, t.raw) < 0)
        return -1;

    u32 *f = (u32 *)t.raw;
    f[T_LFLAG] |= (u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] |= (u32)(ICRNL | BRKINT | IXON | IUTF8);
    f[T_OFLAG] |= (u32)(OPOST | ONLCR);

    /* ISIG above is what makes Ctrl-C a signal instead of a byte, and
     * that is the point of this function. It also makes Ctrl-Z one, and
     * Ctrl-Z is a trap here: SIGTSTP stops the foreground process, and
     * nothing in this system can start a stopped process again. There is
     * no job control - no `fg`, no `bg` - so a stopped command would sit
     * there forever with the shell waiting on it, and the terminal would
     * be dead with no key that fixes it.
     *
     * So the suspend key is disabled outright. Ctrl-Z does nothing,
     * which is the honest behaviour for a shell that cannot resume
     * anything, and it leaves Ctrl-C - the one that has to work - as the
     * way out of a command that will not stop. */
    t.raw[T_CC + VSUSP] = 0;

    return lp_ioctl(fd, TCSETS, t.raw);
}

/* The same, minus the output processing. See the header. */
long lp_term_cbreak(int fd, lp_termios_t *saved)
{
    long r = lp_ioctl(fd, TCGETS, saved->raw);
    if (r < 0)
        return r;

    lp_termios_t t = *saved;
    u32 *f = (u32 *)t.raw;

    f[T_LFLAG] &= ~(u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] &= ~(u32)(IXON | ICRNL | BRKINT | ISTRIP | INLCR);
    f[T_IFLAG] |= IUTF8;
    /* OPOST is deliberately left alone. */

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
int  lp_getuid(void)          { return (int)sys_call0(SYS_getuid); }

/* CLOCK_MONOTONIC. Counts from an arbitrary point - only differences
 * between two readings mean anything, which is exactly what timing
 * something needs. */
#define CLOCK_MONOTONIC 1

s64 lp_monotonic_ms(void)
{
    s64 ts[2] = { 0, 0 };           /* { tv_sec, tv_nsec } */
    if (sys_call2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)ts) < 0)
        return 0;
    return ts[0] * 1000 + ts[1] / 1000000;
}

/* ── Signals ──────────────────────────────────────────────────────────
 * The kernel's struct sigaction on arm64, which has no sa_restorer:
 *
 *    0  sa_handler   SIG_DFL is 0, SIG_IGN is 1
 *    8  sa_flags
 *   16  sa_mask      one 64-bit word, hence the size argument of 8
 *
 * A real handler would need a restorer to return through. SIG_DFL and
 * SIG_IGN never run any of our code, so there is nothing to return
 * from and nothing else to fill in. */
#define SA_SIZE      24
#define SA_HANDLER    0
#define SA_MASK_SIZE  8

static long set_disposition(int sig, unsigned long handler)
{
    u8 act[SA_SIZE];
    memset(act, 0, sizeof(act));
    *(unsigned long *)(act + SA_HANDLER) = handler;

    return sys_call4(SYS_rt_sigaction, (long)sig, (long)act, 0, SA_MASK_SIZE);
}

/* TIOCSCTTY: "make this terminal mine". The argument is 0 - 1 would
 * mean "steal it from whoever has it", which needs CAP_SYS_ADMIN and is
 * never what we want: if something else owns the console, taking it is
 * how you end up with two shells reading the same keystrokes. */
#define TIOCSCTTY 0x540E

long lp_term_make_controlling(int fd)
{
    return lp_ioctl(fd, TIOCSCTTY, 0);
}

long lp_signal_ignore(int sig)  { return set_disposition(sig, 1); }
long lp_signal_default(int sig) { return set_disposition(sig, 0); }

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
/* The pgid, session, parent and tty of a process. See unistd.h for why
 * this is not three lines of strtok at each call site. */
bool lp_proc_ids(pid_t pid, pid_t *ppid, pid_t *pgid, pid_t *sid,
                 int *tty_nr)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);

    char buf[512];
    if (proc_read(path, buf, sizeof buf) <= 0)
        return false;

    /* Field 2 is "(name)" and the name may contain ')' and spaces, so
     * the only safe anchor is the last ')' in the whole line. */
    char *p = NULL;
    for (char *c = buf; *c; c++)
        if (*c == ')')
            p = c;
    if (!p)
        return false;
    p++;

    /* After the ')': state, ppid, pgrp, session, tty_nr, ... */
    long v[5] = { 0, 0, 0, 0, 0 };
    int got = 0;
    while (got < 5 && *p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (got == 0) {                 /* state is a letter, not a number */
            while (*p && *p != ' ')
                p++;
            got++;
            continue;
        }
        char *end = p;
        long n = strtol(p, &end, 10);
        if (end == p)
            return false;               /* not a number where one must be */
        v[got++] = n;
        p = end;
    }
    if (got < 5)
        return false;

    if (ppid)   *ppid   = (pid_t)v[1];
    if (pgid)   *pgid   = (pid_t)v[2];
    if (sid)    *sid    = (pid_t)v[3];
    if (tty_nr) *tty_nr = (int)v[4];
    return true;
}

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

/* ── SHA-256 ──────────────────────────────────────────────────────────
 *
 * A hundred lines, no dependencies, and the one thing standing between
 * "the bytes arrived" and "the bytes are the ones that were meant to".
 * pkg checks every download with it and sha256sum exposes it. */
typedef struct {
    u32 h[8];
    u64 len;
    u8  buf[64];
    int used;
} sha256_t;

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static u32 ror(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(sha256_t *s, const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) |
               ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; i++) {
        u32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->used = 0;
}

static void sha256_update(sha256_t *s, const u8 *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - (size_t)s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += (int)take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha256_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha256_final(sha256_t *s, char *hex)
{
    u64 bits = s->len * 8;

    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    u8 zero = 0;
    while (s->used != 56)
        sha256_update(s, &zero, 1);

    u8 lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (56 - i * 8));
    /* Straight into the block: update would count these bytes again. */
    memcpy(s->buf + 56, lenb, 8);
    sha256_block(s, s->buf);

    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 4; b++) {
            u8 byte = (u8)(s->h[i] >> (24 - b * 8));
            *hex++ = digits[byte >> 4];
            *hex++ = digits[byte & 15];
        }
    *hex = '\0';
}

/* The hash of a file, as 64 hex characters. false if it cannot be read. */
bool lp_sha256_file(const char *path, char *hex)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;

    sha256_t s;
    sha256_init(&s);

    static u8 buf[8192];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        sha256_update(&s, buf, (size_t)n);
    }
    lp_close((int)fd);

    sha256_final(&s, hex);
    return true;
}

/* ── Users and groups ─────────────────────────────────────────────────
 *
 * /etc/passwd is "name:x:uid:gid:comment:home:shell" and /etc/group is
 * "name:x:gid:members". Both are read line by line every time. There is
 * no cache because there is nothing to cache: the files have a handful
 * of lines and live in RAM already. */

int lp_getgid(void)
{
    /* There is no getgid in the asm-generic table under a different
     * name; 176 is getgid on arm64. */
    return (int)sys_call0(176);
}

long lp_setuid(uid_t uid)  { return sys_call1(SYS_setuid, (long)uid); }
long lp_setgid(gid_t gid)  { return sys_call1(SYS_setgid, (long)gid); }

long lp_setgroups(int n, const gid_t *list)
{
    return sys_call2(SYS_setgroups, (long)n, (long)list);
}

long lp_chown(const char *path, uid_t uid, gid_t gid)
{
    return sys_call5(SYS_fchownat, AT_FDCWD, (long)path,
                     (long)uid, (long)gid, 0);
}

/* Split "a:b:c" in place, returning how many fields were found. */
static int split_colons(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *colon = strchr(p, ':');
        if (!colon)
            break;
        *colon = '\0';
        p = colon + 1;
    }
    return n;
}

static bool passwd_scan(const char *want_name, int want_uid, lp_user_t *out)
{
    long fd = lp_open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    bool found = false;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *f[8];
        int n = split_colons(line, f, 8);
        if (n < 7)
            continue;

        int uid = atoi(f[2]);
        if (want_name ? (strcmp(f[0], want_name) != 0) : (uid != want_uid))
            continue;

        strlcpy(out->name,  f[0], sizeof(out->name));
        out->uid = (uid_t)uid;
        out->gid = (gid_t)atoi(f[3]);
        strlcpy(out->home,  f[5], sizeof(out->home));
        strlcpy(out->shell, f[6], sizeof(out->shell));
        found = true;
        break;
    }

    lp_close((int)fd);
    return found;
}

bool lp_user_by_name(const char *name, lp_user_t *out)
{
    return passwd_scan(name, 0, out);
}

bool lp_user_by_uid(uid_t uid, lp_user_t *out)
{
    return passwd_scan(NULL, (int)uid, out);
}

void lp_group_name(gid_t gid, char *out, size_t n)
{
    snprintf(out, n, "%d", (int)gid);      /* the fallback is the number */

    long fd = lp_open("/etc/group", O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *f[6];
        if (split_colons(line, f, 6) < 3)
            continue;
        if ((gid_t)atoi(f[2]) == gid) {
            strlcpy(out, f[0], n);
            break;
        }
    }
    lp_close((int)fd);
}

bool lp_group_by_name(const char *name, gid_t *out)
{
    long fd = lp_open("/etc/group", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    bool found = false;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *f[6];
        if (split_colons(line, f, 6) < 3)
            continue;
        if (strcmp(f[0], name) == 0) {
            *out = (gid_t)atoi(f[2]);
            found = true;
            break;
        }
    }
    lp_close((int)fd);
    return found;
}

/* ── Writing to the log ───────────────────────────────────────────────
 *
 * logd collects two sources: the kernel's ring buffer and a datagram
 * socket at /dev/log. Nothing in this system ever wrote to either, so
 * /data/log/messages held kernel lines and nothing else - every message
 * from init, from rc and from guard went to the console and was gone
 * the moment it scrolled. A board that had been broken into and one
 * that had not produced identical logs, and guard's record of what it
 * killed and why did not survive the reboot that followed.
 *
 * /dev/kmsg rather than the socket: it is one write with no connection
 * to set up, it works before logd is running, and it puts the line in
 * `dmesg` as well. The fd is kept open because the callers are daemons
 * that will use it again.
 *
 * Failure is silent on purpose. This is called from the paths that
 * handle a machine already in trouble, and a logger that complains
 * about not being able to log would only make the console worse. */
void lp_log(const char *tag, const char *msg)
{
    static int kfd = -2;              /* -2 = not tried yet, -1 = no good */

    if (kfd == -2) {
        long fd = lp_open("/dev/kmsg", O_WRONLY, 0);
        kfd = (fd < 0) ? -1 : (int)fd;
    }
    if (kfd < 0)
        return;

    char line[512];
    int  n = snprintf(line, sizeof line, "%s: %s", tag, msg);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;

    /* One write per record: /dev/kmsg splits on write boundaries, not
     * on newlines, and a trailing newline would be printed literally. */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    if (n > 0)
        lp_write(kfd, line, (size_t)n);
}
