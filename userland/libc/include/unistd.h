/* unistd.h - files and processes.
 * The kernel's -errno is passed straight through; there is no global errno.
 * A negative return is an error, and its negation is the errno. */
#ifndef _LP_UNISTD_H
#define _LP_UNISTD_H

#include "types.h"

/* The special dirfd for openat: "relative to the current directory" */
#define AT_FDCWD            (-100)
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_NOFOLLOW 0x100

/* open flags (asm-generic) */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 040000
#define O_CLOEXEC   02000000

/* The standard file descriptors */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* Signals */
#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17

/* mount flags */
#define MS_NOSUID   2
#define MS_NODEV    4
#define MS_NOEXEC   8
#define MS_RDONLY   1
/* bind: make an existing directory visible at a second place as well.
 * No filesystem type is given; it keeps the original's. */
#define MS_BIND     4096

/* reboot magic numbers */
#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_MAGIC2     672274793
#define LINUX_REBOOT_CMD_RESTART  0x01234567
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC

extern char **environ;

/* ── Files ── */
long  lp_open(const char *path, int flags, mode_t mode);
long  lp_close(int fd);
long  lp_read(int fd, void *buf, size_t n);
long  lp_write(int fd, const void *buf, size_t n);
long  lp_lseek(int fd, off_t off, int whence);
long  lp_dup(int fd);
long  lp_dup2(int oldfd, int newfd);
long  lp_pipe(int fds[2]);
long  lp_unlink(const char *path);
long  lp_mkdir(const char *path, mode_t mode);
long  lp_rmdir(const char *path);
long  lp_chdir(const char *path);
long  lp_getcwd(char *buf, size_t n);
long  lp_access(const char *path, int mode);
/* Read directory entries. Returns the bytes filled in, 0 at the end. */
long  sys_getdents(int fd, void *buf, size_t size);
long  lp_ioctl(int fd, unsigned long req, void *arg);

/* ── The terminal ──
 *
 * A program that draws its own screen, like the editor, has to put the
 * terminal in raw mode. In the default canonical mode the kernel holds a
 * whole line until Enter and handles backspace itself, which leaves no
 * way to react to a single arrow key.
 *
 * struct termios is 36 bytes on arm64. We do not use every field, so it
 * is treated as an opaque blob for saving and restoring. */
typedef struct { u8 raw[64]; } lp_termios_t;

/* Save the current settings into saved and switch to raw. 0 on success. */
long  lp_term_raw(int fd, lp_termios_t *saved);
/* Restore from saved. This must run before the program exits, or the
 * shell is left unable to read input. */
long  lp_term_restore(int fd, const lp_termios_t *saved);
/* Terminal size. Falls back to 80x24 and returns -1 if unknown. */
long  lp_term_size(int fd, int *rows, int *cols);

/* Tell the kernel's line editor that input is UTF-8 (IUTF8), so
 * backspace erases a whole Hangul character rather than one byte. */
long  lp_term_set_utf8(int fd);
bool  lp_exists(const char *path);
bool  lp_is_dir(const char *path);
long  lp_rename(const char *from, const char *to);
long  lp_chmod(const char *path, mode_t mode);
long  lp_symlink(const char *target, const char *linkpath);
long  lp_readlink(const char *path, char *buf, size_t n);

/* The kernel's struct stat is 128 bytes on arm64 and most of it is
 * fields we never use. This carries only what we need. */
typedef struct {
    u32 mode;        /* file type (S_IF*) plus permissions */
    u64 size;
} lp_stat_t;

#define LP_S_IFMT   0170000
#define LP_S_IFDIR  0040000
#define LP_S_IFREG  0100000
#define LP_S_IFLNK  0120000

/* With follow_symlink=false this looks at the link itself (lstat). */
long  lp_stat(const char *path, lp_stat_t *out, bool follow_symlink);

/* ── Processes ── */
pid_t lp_fork(void);                 /* built on clone(SIGCHLD) */
long  lp_execve(const char *path, char *const argv[], char *const envp[]);
pid_t lp_wait(int *status);
pid_t lp_waitpid(pid_t pid, int *status, int options);
pid_t lp_getpid(void);
long  lp_setsid(void);
long  lp_kill(pid_t pid, int sig);
void  lp_exit(int code) __attribute__((noreturn));
long  lp_sleep_ms(long ms);

/* ── Time ── */
/* Seconds since 1970-01-01. 0 on failure. */
s64   lp_time(void);
/* Set the system clock. Root only. */
long  lp_settime(s64 unix_seconds);

/* Broken-down time. Unlike struct tm this holds what a person reads:
 * year is not offset from 1900 and mon starts at 1, so nothing to confuse. */
typedef struct {
    int year;    /* e.g. 2026 */
    int mon;     /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int min;     /* 0-59 */
    int sec;     /* 0-60 */
    int wday;    /* 0 = Sunday */
} lp_tm_t;

/* Unix seconds -> broken-down time, in UTC. */
void  lp_gmtime(s64 t, lp_tm_t *out);
/* Broken-down UTC time -> unix seconds. wday is ignored. */
s64   lp_timegm(const lp_tm_t *tm);

/* ── System ── */
long  lp_mount(const char *src, const char *tgt, const char *fstype,
               unsigned long flags, const void *data);
long  lp_reboot(int cmd);
long  lp_swapon(const char *path, int flags);
long  lp_swapoff(const char *path);

/* Read a one-line value out of /proc. Returns the bytes read. */
long  proc_read(const char *path, char *buf, size_t size);
/* Find a value in the "name:   number kB" format /proc/meminfo uses.
 * -1 if it is not there. */
long  proc_find_kv(const char *text, const char *key);
long  lp_sync(void);
long  lp_uname(void *buf);
long  lp_getrandom(void *buf, size_t n, unsigned flags);           /* struct utsname is 390 bytes */

/* Decoding a wait status */
#define LP_WIFEXITED(s)    (((s) & 0x7F) == 0)
#define LP_WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define LP_WIFSIGNALED(s)  ((((s) & 0x7F) + 1) >> 1 > 0)
#define LP_WTERMSIG(s)     ((s) & 0x7F)

#endif /* _LP_UNISTD_H */
