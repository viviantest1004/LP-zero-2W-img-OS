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

/* ── open flags ──
 *
 * The low ones are the same everywhere. O_DIRECTORY is not, and that is
 * a trap worth spelling out: arm and arm64 carry their own fcntl.h that
 * renumbers the upper flags, so O_DIRECTORY is 040000 there and 0200000
 * on x86-64 - where 040000 means O_DIRECT instead. Building the arm64
 * value into an x86-64 program does not fail to compile and does not
 * fail to open; it asks for unbuffered I/O on a directory, and `ls`
 * quietly stops being able to list anything. O_NOFOLLOW and O_DIRECT
 * move for the same reason. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_CLOEXEC   02000000

#if defined(__x86_64__)
#  define O_DIRECTORY 0200000
#  define O_NOFOLLOW  0400000
#  define O_DIRECT    040000
#  define O_LARGEFILE 0100000
#else   /* arm64 */
#  define O_DIRECTORY 040000
#  define O_NOFOLLOW  0100000
#  define O_DIRECT    0200000
#  define O_LARGEFILE 0400000
#endif

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
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGQUIT  3

/* ── Signals ──
 * Only two things are needed here, and neither runs a handler:
 *
 *   ignore    the interactive shell ignores Ctrl-C, so that interrupting
 *             a command does not take the shell down with it
 *   default   a child undoes that before it runs a command, so Ctrl-C
 *             reaches the thing you meant to interrupt
 *
 * Running an actual handler would need a return trampoline and a
 * signal frame, and nothing here has ever wanted one. */
long  lp_signal_ignore(int sig);
long  lp_signal_default(int sig);
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20      /* Ctrl-Z */
#define SIGTTIN 21
#define SIGTTOU 22

/* mount flags */
#define MS_NOSUID   2
#define MS_NODEV    4
#define MS_NOEXEC   8
#define MS_RDONLY   1
/* bind: make an existing directory visible at a second place as well.
 * No filesystem type is given; it keeps the original's. */
#define MS_BIND     4096
#define MS_REMOUNT    32
/* Move a mount somewhere else without unmounting it. The one thing that
 * makes switch_root possible: the real root is mounted under the
 * initramfs, and then moved to / with everything already on it still
 * open. */
#define MS_MOVE     8192

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
/* lp_lseek 의 whence. 리눅스는 어느 아키텍처에서나 같은 값을 쓴다. */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
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

/* Like lp_term_raw, but the kernel still turns \n into a carriage
 * return and a line feed on the way out (OPOST stays on).
 *
 * Full raw mode is right for a program that positions every character
 * itself - an editor. It is wrong for one that prints ordinary lines and
 * only wants keys without waiting for Enter: there, \n moves down a row
 * and leaves the column alone, so every line starts where the last one
 * ended and the screen turns into a staircase. */
long  lp_term_cbreak(int fd, lp_termios_t *saved);

/* Put the terminal back to something usable - echo on, line editing on,
 * newlines that return to column one. For after a program that owned the
 * terminal died without tidying up. */
long  lp_term_sane(int fd);
/* Restore from saved. This must run before the program exits, or the
 * shell is left unable to read input. */
long  lp_term_restore(int fd, const lp_termios_t *saved);

/* ── Making a terminal the controlling terminal ──
 *
 * setsid() alone leaves a process with NO controlling terminal, and a
 * terminal with no foreground process group generates no signals at
 * all: the line discipline has nobody to send them to, so Ctrl-C is
 * silently nothing. Reopening the device is not enough either -
 * /dev/console specifically can never become a controlling terminal,
 * because the kernel forces O_NOCTTY for major 5 minor 1. So it has to
 * be asked for, explicitly, on the real device.
 *
 * Call this in the child, after setsid() and after the device is open.
 * Returns the kernel's -errno; -EPERM means somebody else already owns
 * this terminal. */
long  lp_term_make_controlling(int fd);
/* Terminal size. Falls back to 80x24 and returns -1 if unknown. */
long  lp_term_size(int fd, int *rows, int *cols);

/* Is this file descriptor a terminal? Asking the kernel for the
 * terminal settings is the test: it succeeds on a terminal and fails
 * with ENOTTY on anything else. Used to tell "somebody is typing at me"
 * from "I am in a pipe", which changes what a program should say. */
bool  lp_isatty(int fd);

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
    u32 nlink;
    uid_t uid;
    gid_t gid;
    s64 mtime;       /* seconds since 1970 */
} lp_stat_t;

#define LP_S_IFMT   0170000
#define LP_S_IFDIR  0040000
#define LP_S_IFREG  0100000
#define LP_S_IFLNK  0120000
#define LP_S_IFCHR  0020000
#define LP_S_IFBLK  0060000
#define LP_S_IFIFO  0010000
#define LP_S_IFSOCK 0140000

/* With follow_symlink=false this looks at the link itself (lstat). */
long  lp_stat(const char *path, lp_stat_t *out, bool follow_symlink);

/* ── Processes ── */
pid_t lp_fork(void);                 /* built on clone(SIGCHLD) */
long  lp_execve(const char *path, char *const argv[], char *const envp[]);
pid_t lp_wait(int *status);
pid_t lp_waitpid(pid_t pid, int *status, int options);
#define WNOHANG 1        /* return at once when the child is still running */
pid_t lp_getpid(void);
/* There is one user on this system and it is root, so this is a
 * formality - but a script asking "am I root" deserves an answer. */
int   lp_getuid(void);
int   lp_getgid(void);
long  lp_setuid(uid_t uid);
long  lp_setgid(gid_t gid);
long  lp_setgroups(int n, const gid_t *list);
/* Owner and group of a file. -1 for either leaves it alone, which is
 * how chgrp is chown with the user left out. */
long  lp_chown(const char *path, uid_t uid, gid_t gid);

/* ── Users ──
 *
 * /etc/passwd and /etc/group, read straight off the disk each time.
 * There is no name service, no cache and no getpwnam: one file, a few
 * lines, and the cost of reading it is a page.
 *
 * /etc is in RAM and is rebuilt from the kernel image at every boot, so
 * a user added on the machine lives in /data/users and is merged back
 * in by /etc/rc. That is the same trick the SSH keys use, and for the
 * same reason. */
typedef struct {
    char name[32];
    uid_t uid;
    gid_t gid;
    char home[64];
    char shell[48];
} lp_user_t;

/* Look one up by name or by number. false when there is no such user. */
bool lp_user_by_name(const char *name, lp_user_t *out);
bool lp_user_by_uid(uid_t uid, lp_user_t *out);
/* The name for a group id, or the number as text when it has none.
 * `out` needs 32 bytes. */
void lp_group_name(gid_t gid, char *out, size_t n);
/* A group id by name. false when there is no such group. */
bool lp_group_by_name(const char *name, gid_t *out);
long  lp_setsid(void);
long  lp_kill(pid_t pid, int sig);
void  lp_exit(int code) __attribute__((noreturn));
long  lp_sleep_ms(long ms);

/* ── Time ── */
/* Seconds since 1970-01-01. 0 on failure. */
s64   lp_time(void);
/* Set the system clock. Root only. */
long  lp_settime(s64 unix_seconds);

/* Milliseconds since the machine started. Unlike lp_time this never
 * jumps: ntp setting the clock does not move it, so it is the one to
 * measure an interval with. */
s64   lp_monotonic_ms(void);

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
/* lp_access 의 mode. 존재만 보려면 F_OK. */
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

/* 마운트를 푼다. flags 에 MNT_DETACH(2) 를 주면 쓰는 사람이 있어도
 * 트리에서 떼어낸다 - 뽑힌 드라이브를 정리할 때 필요하다. */
#define MNT_DETACH  2
long  lp_umount(const char *tgt, int flags);

/* 루트를 옮긴다. apt 가 데비안 트리 안에서 돌 때 쓴다 - dpkg 는
 * /var/lib/dpkg 같은 절대경로를 코드에 박고 있어서, 그 트리를 진짜
 * 루트로 보여주는 것 말고는 방법이 없다. */
long  lp_chroot(const char *path);

long  lp_mount(const char *src, const char *tgt, const char *fstype,
               unsigned long flags, const void *data);
long  lp_reboot(int cmd);

/* Run fn() when this signal arrives.
 *
 * The only other dispositions here are SIG_DFL and SIG_IGN, which need
 * nothing from us because they never run our code. A real handler does:
 * it has to return somewhere, and on x86-64 the kernel will not deliver
 * one without a restorer to return through. See set_disposition.
 *
 * SA_RESTART is deliberately NOT set. init waits in wait() with no
 * timeout, and the point of sending it a signal is to make that wait
 * return so the main loop can look at what changed. */
long  lp_signal_handler(int sig, void (*fn)(int));
long  lp_swapon(const char *path, int flags);
long  lp_swapoff(const char *path);

/* Read a one-line value out of /proc. Returns the bytes read. */
long  proc_read(const char *path, char *buf, size_t size);
/* Find a value in the "name:   number kB" format /proc/meminfo uses.
 * -1 if it is not there. */
long  proc_find_kv(const char *text, const char *key);
long  lp_sync(void);

/* SHA-256 of a file, written to `hex` as 64 characters and a NUL.
 * false when the file cannot be read. */
bool  lp_sha256_file(const char *path, char *hex);

/* Scheduling priority ("nice"): -20 gets the CPU first, 19 last, 0 is
 * the default. Lowering it needs root. */
long  lp_setpriority(pid_t pid, int nice_value);
/* The nice value of a process, or 0 when it cannot be read. */
int   lp_getpriority(pid_t pid);

/* A per-process resource ceiling. `resource` is one of LP_RLIMIT_*.
 * Returns the kernel's -errno.
 *
 * Worth knowing before relying on this: root is exempt from
 * RLIMIT_NPROC. A process with CAP_SYS_ADMIN or CAP_SYS_RESOURCE - which
 * every root process here has - passes the fork check regardless of the
 * limit. So this bounds what a user can do, not what root can do. The
 * bound on root is kernel.pid_max, which /etc/rc sets. */
long  lp_setrlimit(int resource, u64 soft, u64 hard);
#define LP_RLIMIT_CPU     0
#define LP_RLIMIT_FSIZE   1
#define LP_RLIMIT_DATA    2
#define LP_RLIMIT_STACK   3
#define LP_RLIMIT_NPROC   6
#define LP_RLIMIT_NOFILE  7

/* The pgid, session id, parent and controlling terminal of a process,
 * read from /proc/<pid>/stat. Any out pointer may be NULL.
 *
 * The reason this is a libc function and not three lines at each call
 * site: field 2 of that file is the process name in parentheses, and the
 * name may itself contain spaces and parentheses - a process can call
 * itself ") 1 (" if it wants to. Splitting on spaces gets the wrong
 * fields for such a process, which is exactly the process you are
 * looking at when you need this. Parsing starts after the LAST ')'.
 *
 * false when the process is gone or the file cannot be parsed. */
bool  lp_proc_ids(pid_t pid, pid_t *ppid, pid_t *pgid, pid_t *sid,
                  int *tty_nr);

/* Free and total bytes of the filesystem holding `path`.
 * "free" is what an unprivileged process may still use. */
long  lp_fs_space(const char *path, u64 *free_bytes, u64 *total_bytes);
long  lp_uname(void *buf);
long  lp_getrandom(void *buf, size_t n, unsigned flags);

/* Put one line in the system log, so it survives the reboot. Console
 * output does not: it goes out the serial port and is gone. Silent on
 * failure - see the note by the definition. */
void  lp_log(const char *tag, const char *msg);           /* struct utsname is 390 bytes */

/* ── "there is nothing for me to do here" ──
 *
 * A supervised service exits with this when the hardware or the setting
 * it exists to work with is absent on this machine - as opposed to
 * failing at something it should have managed. init does not restart it
 * and does not treat it as a fault.
 *
 * The case that made this necessary: the watchdog service on a virtual
 * machine, where there is no watchdog device at all. Exiting 1 made init
 * restart it, back off, restart it again and print the same complaint a
 * dozen times before giving up - a fault report, repeated, about
 * something that is not a fault and will never change while the machine
 * is running. */
#define LP_EXIT_NO_HARDWARE  78

/* Decoding a wait status */
#define LP_WIFEXITED(s)    (((s) & 0x7F) == 0)
#define LP_WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define LP_WIFSIGNALED(s)  ((((s) & 0x7F) + 1) >> 1 > 0)
#define LP_WTERMSIG(s)     ((s) & 0x7F)

#endif /* _LP_UNISTD_H */
