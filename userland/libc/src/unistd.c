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

/* ── struct stat, by offset ──
 *
 * Rather than declaring the whole thing we read the handful of fields we
 * use at fixed offsets, so nothing depends on how a compiler pads a
 * struct. The offsets are not the same on both machines: arm64 uses the
 * asm-generic layout, x86-64 carries its own older one where st_nlink
 * is 64 bits and comes before st_mode instead of after it.
 *
 *              arm64   x86-64
 *   st_mode      16      24
 *   st_nlink     20      16   (32-bit on arm64, 64-bit on x86-64)
 *   st_uid       24      28
 *   st_gid       28      32
 *   st_size      48      48
 *   st_mtime     88      88
 *
 * st_size and st_mtime happen to land in the same place on both, which
 * is luck rather than design - they are written out here anyway so that
 * nobody has to rediscover it. */
#if defined(__x86_64__)
#  define STAT_BUF_SIZE  144
#  define STAT_MODE_OFF   24
#  define STAT_NLINK_OFF  16
#  define STAT_UID_OFF    28
#  define STAT_GID_OFF    32
#else
#  define STAT_BUF_SIZE  128
#  define STAT_MODE_OFF   16
#  define STAT_NLINK_OFF  20
#  define STAT_UID_OFF    24
#  define STAT_GID_OFF    28
#endif
#define STAT_MTIME_OFF    88
/* st_blocks: 512-byte units the file actually occupies, which is not
 * the same as its size - a sparse file uses fewer, and a small file on
 * a filesystem with big blocks uses more. `ls -l` prints the sum as
 * "total", so guessing it from the size gave a different number from
 * every other ls. Both architectures put it at 64. */
#define STAT_BLOCKS_OFF   64
/* dev, ino, atime and ctime land in the same place on both; only rdev
 * and the width of st_blksize differ. */
#define STAT_DEV_OFF       0
#define STAT_INO_OFF       8
#define STAT_BLKSIZE_OFF  56
#define STAT_ATIME_OFF    72
#define STAT_CTIME_OFF   104
#if defined(__x86_64__)
#  define STAT_RDEV_OFF   40
#else
#  define STAT_RDEV_OFF   32
#endif
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

/* statx, laid out the way the kernel writes it. It is here for one
 * field - the creation time - which newfstatat cannot report at all and
 * which `stat` prints on every other Linux. Everything else it returns
 * we already had. Kernels before 4.11 have no statx, so a failure here
 * falls back rather than failing the call. */
#define STATX_BASIC_STATS 0x7ff
#define STATX_BTIME       0x800
#define STATX_MASK_OFF      0
#define STATX_BLKSIZE_OFF   4
#define STATX_NLINK_OFF    16
#define STATX_UID_OFF      20
#define STATX_GID_OFF      24
#define STATX_MODE_OFF     28
#define STATX_INO_OFF      32
#define STATX_SIZE_OFF     40
#define STATX_BLOCKS_OFF   48
#define STATX_ATIME_OFF    64
#define STATX_BTIME_OFF    80
#define STATX_CTIME_OFF    96
#define STATX_MTIME_OFF   112
#define STATX_RDEV_MAJ_OFF 128
#define STATX_RDEV_MIN_OFF 132
#define STATX_DEV_MAJ_OFF 136
#define STATX_DEV_MIN_OFF 140

static u64 makedev(u32 maj, u32 min)
{
    return ((u64)(maj & 0xfff) << 8) | (u64)(min & 0xff) |
           ((u64)(maj & ~0xfffu) << 32) | ((u64)(min & ~0xffu) << 12);
}

static long try_statx(const char *path, lp_stat_t *out, bool follow_symlink)
{
    u8 buf[256];
    memset(buf, 0, sizeof buf);
    long r = sys_call5(SYS_statx, AT_FDCWD, (long)path,
                       follow_symlink ? 0 : AT_SYMLINK_NOFOLLOW,
                       STATX_BASIC_STATS | STATX_BTIME, (long)buf);
    if (r < 0)
        return r;

    u32 mask   = *(u32 *)(buf + STATX_MASK_OFF);
    out->mode  = *(u16 *)(buf + STATX_MODE_OFF);
    out->size  = *(u64 *)(buf + STATX_SIZE_OFF);
    out->nlink = *(u32 *)(buf + STATX_NLINK_OFF);
    out->uid   = *(u32 *)(buf + STATX_UID_OFF);
    out->gid   = *(u32 *)(buf + STATX_GID_OFF);
    out->blocks  = *(u64 *)(buf + STATX_BLOCKS_OFF);
    out->blksize = *(u32 *)(buf + STATX_BLKSIZE_OFF);
    out->ino     = *(u64 *)(buf + STATX_INO_OFF);
    out->atime = *(s64 *)(buf + STATX_ATIME_OFF);
    out->mtime = *(s64 *)(buf + STATX_MTIME_OFF);
    out->ctime = *(s64 *)(buf + STATX_CTIME_OFF);
    out->atime_ns = *(u32 *)(buf + STATX_ATIME_OFF + 8);
    out->mtime_ns = *(u32 *)(buf + STATX_MTIME_OFF + 8);
    out->ctime_ns = *(u32 *)(buf + STATX_CTIME_OFF + 8);
    out->btime    = (mask & STATX_BTIME) ? *(s64 *)(buf + STATX_BTIME_OFF) : 0;
    out->btime_ns = (mask & STATX_BTIME) ? *(u32 *)(buf + STATX_BTIME_OFF + 8) : 0;
    out->has_btime = (mask & STATX_BTIME) != 0;
    out->rdev = makedev(*(u32 *)(buf + STATX_RDEV_MAJ_OFF),
                        *(u32 *)(buf + STATX_RDEV_MIN_OFF));
    out->dev  = makedev(*(u32 *)(buf + STATX_DEV_MAJ_OFF),
                        *(u32 *)(buf + STATX_DEV_MIN_OFF));
    return 0;
}

long lp_stat(const char *path, lp_stat_t *out, bool follow_symlink)
{
    if (try_statx(path, out, follow_symlink) == 0)
        return 0;

    u8 buf[STAT_BUF_SIZE];
    long r = sys_call4(SYS_newfstatat, AT_FDCWD, (long)path, (long)buf,
                       follow_symlink ? 0 : AT_SYMLINK_NOFOLLOW);
    if (r < 0)
        return r;
    out->mode  = *(u32 *)(buf + STAT_MODE_OFF);
    out->size  = *(u64 *)(buf + STAT_SIZE_OFF);
#if defined(__x86_64__)
    out->nlink = (u32)*(u64 *)(buf + STAT_NLINK_OFF);
#else
    out->nlink = *(u32 *)(buf + STAT_NLINK_OFF);
#endif
    out->uid   = *(u32 *)(buf + STAT_UID_OFF);
    out->gid   = *(u32 *)(buf + STAT_GID_OFF);
    out->mtime = *(s64 *)(buf + STAT_MTIME_OFF);
    out->blocks = *(u64 *)(buf + STAT_BLOCKS_OFF);
    out->dev   = *(u64 *)(buf + STAT_DEV_OFF);
    out->ino   = *(u64 *)(buf + STAT_INO_OFF);
    out->rdev  = *(u64 *)(buf + STAT_RDEV_OFF);
#if defined(__x86_64__)
    out->blksize = *(u64 *)(buf + STAT_BLKSIZE_OFF);
#else
    out->blksize = *(u32 *)(buf + STAT_BLKSIZE_OFF);
#endif
    out->atime = *(s64 *)(buf + STAT_ATIME_OFF);
    out->ctime = *(s64 *)(buf + STAT_CTIME_OFF);
    out->atime_ns = (u32)*(u64 *)(buf + STAT_ATIME_OFF + 8);
    out->mtime_ns = (u32)*(u64 *)(buf + STAT_MTIME_OFF + 8);
    out->ctime_ns = (u32)*(u64 *)(buf + STAT_CTIME_OFF + 8);
    out->btime = 0;
    out->btime_ns = 0;
    out->has_btime = false;
    return 0;
}

/* Set a file's length. truncate and `> file` both want it, and it is
 * the only way to make a sparse file without writing the whole thing. */
long lp_ftruncate(int fd, s64 length)
{
    return sys_call2(SYS_ftruncate, fd, (long)length);
}

/* A hard link: a second name for the same inode. */
long lp_link(const char *from, const char *to)
{
    return sys_call5(SYS_linkat, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0);
}

/* A named pipe, or any other node the caller has a mode for. */
long lp_mknod(const char *path, mode_t mode, u64 dev)
{
    return sys_call4(SYS_mknodat, AT_FDCWD, (long)path, mode, (long)dev);
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

long lp_umount(const char *tgt, int flags)
{
    return sys_call2(SYS_umount2, (long)tgt, flags);
}

long lp_chroot(const char *path)
{
    return sys_call1(SYS_chroot, (long)path);
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
 *
 * The kernel's struct sigaction, which is not the same shape on the two
 * machines:
 *
 *   arm64                     x86-64
 *    0  sa_handler             0  sa_handler
 *    8  sa_flags               8  sa_flags
 *   16  sa_mask               16  sa_restorer
 *   (24 bytes)                24  sa_mask
 *                             (32 bytes)
 *
 * arm64 does not define SA_RESTORER, so that field is simply absent and
 * sa_mask moves up. Getting this wrong is not a compile error: the
 * kernel would read sa_mask from eight bytes past the end of our
 * buffer, and the mask a signal is delivered under would be whatever
 * happened to be on the stack.
 *
 * SIG_DFL and SIG_IGN never run any of our code, so no restorer is
 * needed to get back from them. It is filled in anyway on x86-64, where
 * the kernel refuses to deliver a caught signal without one - so that
 * the day somebody adds a real handler here, it works rather than
 * killing the process in a way that takes an afternoon to explain. */
#if defined(__x86_64__)
#  define SA_SIZE       32
#  define SA_MASK_OFF   24
#  define SA_RESTORER_OFF 16
#  define SA_RESTORER   0x04000000UL
extern void lp_sigreturn_trampoline(void);
#else
#  define SA_SIZE       24
#  define SA_MASK_OFF   16
#endif
#define SA_HANDLER    0
#define SA_FLAGS      8
#define SA_MASK_SIZE  8

static long set_disposition(int sig, unsigned long handler)
{
    u8 act[SA_SIZE];
    memset(act, 0, sizeof(act));
    *(unsigned long *)(act + SA_HANDLER) = handler;

#if defined(__x86_64__)
    *(unsigned long *)(act + SA_FLAGS)        = SA_RESTORER;
    *(unsigned long *)(act + SA_RESTORER_OFF) =
        (unsigned long)&lp_sigreturn_trampoline;
#endif

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

long lp_signal_handler(int sig, void (*fn)(int))
{
    u8 act[SA_SIZE];
    memset(act, 0, sizeof(act));
    *(unsigned long *)(act + SA_HANDLER) = (unsigned long)fn;

#if defined(__x86_64__)
    *(unsigned long *)(act + SA_FLAGS)        = SA_RESTORER;
    *(unsigned long *)(act + SA_RESTORER_OFF) =
        (unsigned long)&lp_sigreturn_trampoline;
#endif

    return sys_call4(SYS_rt_sigaction, (long)sig, (long)act, 0, SA_MASK_SIZE);
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

/* statfs, in full. df needs every field of it: the total is one number,
 * what is used is another, and what an ordinary process may still write
 * is a third - ext4 keeps a few percent back for root, and reporting
 * the difference between total and used as "free" overstates it by
 * exactly the reserve that keeps a full disk recoverable. */
#define STATFS_OFF_TYPE     0
#define STATFS_OFF_BFREE   24
#define STATFS_OFF_FILES   40
#define STATFS_OFF_FFREE   48
#define STATFS_OFF_NAMELEN 64
#define STATFS_OFF_FRSIZE  72

long lp_statfs(const char *path, lp_statfs_t *out)
{
    u8 buf[STATFS_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    long rc = sys_call2(SYS_statfs, (long)path, (long)buf);
    if (rc < 0)
        return rc;

    out->type    = *(u64 *)(buf + STATFS_OFF_TYPE);
    out->bsize   = *(u64 *)(buf + STATFS_OFF_BSIZE);
    out->blocks  = *(u64 *)(buf + STATFS_OFF_BLOCKS);
    out->bfree   = *(u64 *)(buf + STATFS_OFF_BFREE);
    out->bavail  = *(u64 *)(buf + STATFS_OFF_BAVAIL);
    out->files   = *(u64 *)(buf + STATFS_OFF_FILES);
    out->ffree   = *(u64 *)(buf + STATFS_OFF_FFREE);
    out->namelen = *(u64 *)(buf + STATFS_OFF_NAMELEN);
    out->frsize  = *(u64 *)(buf + STATFS_OFF_FRSIZE);
    if (out->frsize == 0) out->frsize = out->bsize;
    return 0;
}

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
    if (n >= (int)sizeof line - 1)
        n = (int)sizeof line - 2;   /* keep a byte for the newline */

    /* One write per record - /dev/kmsg splits on write boundaries - and
     * the record has to end in a newline.
     *
     * This is not cosmetic. A printk whose text does not end in '\n' is
     * a *continuation*: the kernel commits it unfinalised, so that a
     * later write can append to it, and nothing - not dmesg, not a
     * reader of /dev/kmsg, not the console - shows it until something
     * else finalises it. Written without the newline, every message
     * here appeared only once the *next* one was logged, and the last
     * message before a quiet spell was never seen at all. That is how
     * guard's "held a core for 30s" line went missing from dmesg while
     * the same text, printed to the console, was right there on screen.
     *
     * The kernel strips the newline again before storing the text, so
     * it is a terminator, not part of the message. */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    if (n <= 0)
        return;
    line[n++] = '\n';
    lp_write(kfd, line, (size_t)n);
}

/* ── Local time ───────────────────────────────────────────────────────
 *
 * See unistd.h for why this is here rather than in `date`.
 *
 * ── Why daylight saving is computed ──
 *
 * The zone table used to say "uses DST - shift by hand in summer", and
 * that is not a time zone, it is a chore with a deadline. Twice a year
 * every timestamp on the machine is an hour wrong until somebody
 * notices, and logs written across the change cannot be read at all.
 *
 * Real tzdata is tens of megabytes and this root lives in RAM. But the
 * rules themselves are four paragraphs and they have been stable for
 * twenty years, so they are code here. A zone that follows none of them
 * is a fixed offset, which is the truth for most of the world.
 */

/* The day of the month of the nth Sunday (nth = -1 means the last). */
static int nth_sunday(int year, int mon, int nth)
{
    lp_tm_t t = { .year = year, .mon = mon, .day = 1,
                  .hour = 0, .min = 0, .sec = 0, .wday = 0 };
    lp_gmtime(lp_timegm(&t), &t);
    int first_sun = 1 + ((7 - t.wday) % 7);      /* wday 0 = Sunday */

    if (nth > 0)
        return first_sun + (nth - 1) * 7;

    static const int LEN[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    int len = LEN[mon];
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        len = 29;
    int d = first_sun;
    while (d + 7 <= len) d += 7;
    return d;
}

/* Unix seconds of a given Y/M/D H:00 UTC. */
static s64 at_utc(int year, int mon, int day, int hour)
{
    lp_tm_t t = { .year = year, .mon = mon, .day = day,
                  .hour = hour, .min = 0, .sec = 0, .wday = 0 };
    return lp_timegm(&t);
}

/* Is daylight saving in force at this instant, under this rule? */
static bool dst_active(lp_dst_t rule, s64 utc, int std_minutes)
{
    if (rule == LP_DST_NONE)
        return false;

    lp_tm_t g;
    lp_gmtime(utc, &g);
    int y = g.year;

    s64 start, end;
    switch (rule) {
    case LP_DST_EU:
        /* Both transitions happen at 01:00 UTC across the whole union,
         * which is what makes this the easy one. */
        start = at_utc(y, 3,  nth_sunday(y, 3, -1), 1);
        end   = at_utc(y, 10, nth_sunday(y, 10, -1), 1);
        return utc >= start && utc < end;

    case LP_DST_US:
        /* 02:00 local standard time in each zone, so the instant in UTC
         * depends on the offset. */
        start = at_utc(y, 3,  nth_sunday(y, 3, 2), 2) - (s64)std_minutes * 60;
        end   = at_utc(y, 11, nth_sunday(y, 11, 1), 2) - (s64)(std_minutes + 60) * 60;
        return utc >= start && utc < end;

    case LP_DST_AU:
        /* Southern hemisphere: summer spans the new year, so the test is
         * "outside the winter gap" rather than "inside a summer range". */
        start = at_utc(y, 10, nth_sunday(y, 10, 1), 2) - (s64)std_minutes * 60;
        end   = at_utc(y, 4,  nth_sunday(y, 4, 1), 3) - (s64)(std_minutes + 60) * 60;
        return utc >= start || utc < end;

    case LP_DST_NZ:
        start = at_utc(y, 9, nth_sunday(y, 9, -1), 2) - (s64)std_minutes * 60;
        end   = at_utc(y, 4, nth_sunday(y, 4, 1), 3) - (s64)(std_minutes + 60) * 60;
        return utc >= start || utc < end;

    default:
        return false;
    }
}

/* The chosen zone. "<minutes> <label> [<rule>] [<summer label>]".
 *
 * Re-read every ten seconds rather than once.
 *
 * Once was wrong for the half of this system that never exits. cron,
 * logd and the supervisor all start at boot and run for months; with a
 * one-shot cache, `date -z Asia/Seoul` would move the clock for every
 * command you typed afterwards and for none of the daemons, until the
 * next reboot. Two clocks on one machine, and the logs are the ones
 * that stay wrong. Ten seconds costs a stat per ten seconds and makes
 * the setting mean what it says. */
static s64      tz_checked  = -1;
static bool     tz_loaded   = false;
static int      tz_std_min  = 0;
static char     tz_std_lab[16] = "UTC";
static char     tz_dst_lab[16] = "UTC";
static lp_dst_t tz_rule     = LP_DST_NONE;

static void tz_load(void)
{
    s64 now = lp_monotonic_ms();
    if (tz_loaded && tz_checked >= 0 && now - tz_checked < 10000)
        return;
    tz_checked = now;
    tz_loaded  = true;

    /* Back to the defaults before re-reading, or a zone that was unset
     * would keep the last one it had. */
    tz_std_min = 0;
    tz_rule    = LP_DST_NONE;
    strlcpy(tz_std_lab, "UTC", sizeof tz_std_lab);
    strlcpy(tz_dst_lab, "UTC", sizeof tz_dst_lab);

    char buf[128];
    buf[0] = '\0';
    /* /data first: on a RAM root it is the half that survives. */
    const char *paths[2] = { "/data/timezone", "/etc/timezone" };
    for (int i = 0; i < 2; i++) {
        long fd = lp_open(paths[i], O_RDONLY, 0);
        if (fd < 0)
            continue;
        long n = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (n > 0) { buf[n] = '\0'; break; }
        buf[0] = '\0';
    }
    if (!buf[0])
        return;

    /* minutes */
    const char *p = buf;
    while (*p == ' ') p++;
    tz_std_min = atoi(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    /* label */
    int k = 0;
    while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof tz_std_lab - 1)
        tz_std_lab[k++] = *p++;
    tz_std_lab[k] = '\0';
    if (!k)
        strlcpy(tz_std_lab, "UTC", sizeof tz_std_lab);
    strlcpy(tz_dst_lab, tz_std_lab, sizeof tz_dst_lab);

    /* rule, optional */
    while (*p == ' ') p++;
    char rule[16] = "";
    k = 0;
    while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof rule - 1)
        rule[k++] = *p++;
    rule[k] = '\0';

    if      (strcmp(rule, "EU") == 0) tz_rule = LP_DST_EU;
    else if (strcmp(rule, "US") == 0) tz_rule = LP_DST_US;
    else if (strcmp(rule, "AU") == 0) tz_rule = LP_DST_AU;
    else if (strcmp(rule, "NZ") == 0) tz_rule = LP_DST_NZ;

    /* The summer label, if one was given. */
    while (*p == ' ') p++;
    k = 0;
    while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof tz_dst_lab - 1)
        tz_dst_lab[k++] = *p++;
    if (k) tz_dst_lab[k] = '\0';
}

int lp_tz_offset(s64 utc)
{
    tz_load();
    return tz_std_min + (dst_active(tz_rule, utc, tz_std_min) ? 60 : 0);
}

const char *lp_tz_label(s64 utc)
{
    tz_load();
    return dst_active(tz_rule, utc, tz_std_min) ? tz_dst_lab : tz_std_lab;
}

void lp_localtime(s64 t, lp_tm_t *out)
{
    lp_gmtime(t + (s64)lp_tz_offset(t) * 60, out);
}

s64 lp_timelocal(const lp_tm_t *tm)
{
    /* The offset depends on the instant, and the instant is what we are
     * computing. One round of feedback settles it everywhere except the
     * hour that daylight saving skips, which has no answer to settle on. */
    s64 guess = lp_timegm(tm);
    guess -= (s64)lp_tz_offset(guess) * 60;
    return guess - ((s64)lp_tz_offset(guess) * 60
                    - (s64)lp_tz_offset(lp_timegm(tm)) * 60);
}

/* ── Which voice the commands speak in ────────────────────────────────
 *
 * See unistd.h. The default is GNU's wording because this machine is
 * used to learn on, and a command that answers differently from the one
 * on Ubuntu teaches something that has to be unlearned later.
 */
static bool       voice_loaded = false;
static lp_voice_t voice_value  = LP_VOICE_GNU;

lp_voice_t lp_voice(void)
{
    if (voice_loaded)
        return voice_value;
    voice_loaded = true;

    char buf[32];
    const char *paths[2] = { "/data/voice", "/etc/voice" };
    for (int i = 0; i < 2; i++) {
        long fd = lp_open(paths[i], O_RDONLY, 0);
        if (fd < 0)
            continue;
        long n = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (buf[0] == 'l' || buf[0] == 'L')      /* "lp" */
            voice_value = LP_VOICE_LP;
        return voice_value;
    }
    return voice_value;
}

/* The errno names people actually see. Not the whole table: the ones
 * missing here print as a number, which is still more than "failed". */
const char *lp_strerror(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case 0:   return "Success";
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 3:   return "No such process";
    case 4:   return "Interrupted system call";
    case 5:   return "Input/output error";
    case 6:   return "No such device or address";
    case 7:   return "Argument list too long";
    case 8:   return "Exec format error";
    case 9:   return "Bad file descriptor";
    case 10:  return "No child processes";
    case 11:  return "Resource temporarily unavailable";
    case 12:  return "Cannot allocate memory";
    case 13:  return "Permission denied";
    case 14:  return "Bad address";
    case 16:  return "Device or resource busy";
    case 17:  return "File exists";
    case 18:  return "Invalid cross-device link";
    case 19:  return "No such device";
    case 20:  return "Not a directory";
    case 21:  return "Is a directory";
    case 22:  return "Invalid argument";
    case 23:  return "Too many open files in system";
    case 24:  return "Too many open files";
    case 25:  return "Inappropriate ioctl for device";
    case 26:  return "Text file busy";
    case 27:  return "File too large";
    case 28:  return "No space left on device";
    case 29:  return "Illegal seek";
    case 30:  return "Read-only file system";
    case 31:  return "Too many links";
    case 32:  return "Broken pipe";
    case 33:  return "Numerical argument out of domain";
    case 34:  return "Numerical result out of range";
    case 36:  return "File name too long";
    case 38:  return "Function not implemented";
    case 39:  return "Directory not empty";
    case 40:  return "Too many levels of symbolic links";
    case 61:  return "No data available";
    case 62:  return "Timer expired";
    case 71:  return "Protocol error";
    case 88:  return "Socket operation on non-socket";
    case 91:  return "Protocol wrong type for socket";
    case 95:  return "Operation not supported";
    case 97:  return "Address family not supported by protocol";
    case 98:  return "Address already in use";
    case 99:  return "Cannot assign requested address";
    case 101: return "Network is unreachable";
    case 104: return "Connection reset by peer";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 113: return "No route to host";
    default:  return NULL;
    }
}

void lp_diag(const char *prog, const char *gnu_before, const char *gnu_after,
             const char *lp_phrase, const char *path, int err)
{
    if (err < 0) err = -err;
    const char *msg = lp_strerror(err);
    char unknown[32];
    if (!msg) {
        snprintf(unknown, sizeof unknown, "Unknown error %d", err);
        msg = unknown;
    }

    if (lp_voice() == LP_VOICE_GNU) {
        if (gnu_before)
            dprintf(STDERR_FILENO, "%s: %s '%s'%s%s: %s\n",
                    prog, gnu_before, path,
                    gnu_after ? " " : "", gnu_after ? gnu_after : "", msg);
        else
            dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, path, msg);
        return;
    }

    /* This system's own voice: the errno number is kept, because it is
     * the thing you look up when the sentence is not enough. */
    dprintf(STDERR_FILENO, "%s: %s: %s (%d)\n",
            prog, path, lp_phrase ? lp_phrase : (msg ? msg : "failed"), err);
}
