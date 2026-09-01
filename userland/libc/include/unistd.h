/* unistd.h - 파일·프로세스 인터페이스.
 * 커널이 돌려주는 -errno 를 그대로 노출한다 (errno 전역을 쓰지 않는다).
 * 반환값이 음수면 오류, -반환값이 errno. */
#ifndef _LP_UNISTD_H
#define _LP_UNISTD_H

#include "types.h"

/* openat 의 dirfd 특수값: "현재 디렉터리 기준" */
#define AT_FDCWD            (-100)
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_NOFOLLOW 0x100

/* open 플래그 (asm-generic) */
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

/* 표준 파일 디스크립터 */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* 시그널 */
#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17

/* mount 플래그 */
#define MS_NOSUID   2
#define MS_NODEV    4
#define MS_NOEXEC   8
#define MS_RDONLY   1
/* bind: 이미 있는 디렉터리를 다른 위치에도 보이게 한다.
 * 파일시스템 타입을 주지 않고 원본의 것을 그대로 쓴다. */
#define MS_BIND     4096

/* reboot 매직 */
#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_MAGIC2     672274793
#define LINUX_REBOOT_CMD_RESTART  0x01234567
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC

extern char **environ;

/* ── 파일 ── */
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
/* 디렉터리 항목을 읽는다. 반환: 채운 바이트 수, 끝이면 0. */
long  sys_getdents(int fd, void *buf, size_t size);
bool  lp_exists(const char *path);
bool  lp_is_dir(const char *path);
long  lp_rename(const char *from, const char *to);
long  lp_chmod(const char *path, mode_t mode);
long  lp_symlink(const char *target, const char *linkpath);
long  lp_readlink(const char *path, char *buf, size_t n);

/* 커널의 struct stat 은 arm64 에서 128바이트이고 대부분 우리가 쓰지
 * 않는 필드다. 필요한 것만 담아 돌려준다. */
typedef struct {
    u32 mode;        /* 종류(S_IF*) + 권한 */
    u64 size;
} lp_stat_t;

#define LP_S_IFMT   0170000
#define LP_S_IFDIR  0040000
#define LP_S_IFREG  0100000
#define LP_S_IFLNK  0120000

/* follow_symlink=false 면 링크 자체를 본다 (lstat). */
long  lp_stat(const char *path, lp_stat_t *out, bool follow_symlink);

/* ── 프로세스 ── */
pid_t lp_fork(void);                 /* clone(SIGCHLD) 로 구현 */
long  lp_execve(const char *path, char *const argv[], char *const envp[]);
pid_t lp_wait(int *status);
pid_t lp_waitpid(pid_t pid, int *status, int options);
pid_t lp_getpid(void);
long  lp_setsid(void);
long  lp_kill(pid_t pid, int sig);
void  lp_exit(int code) __attribute__((noreturn));
long  lp_sleep_ms(long ms);

/* ── 시각 ── */
/* 1970-01-01 부터의 초. 실패하면 0. */
s64   lp_time(void);
/* 시스템 시각을 맞춘다. root 만 된다. */
long  lp_settime(s64 unix_seconds);

/* ── 시스템 ── */
long  lp_mount(const char *src, const char *tgt, const char *fstype,
               unsigned long flags, const void *data);
long  lp_reboot(int cmd);
long  lp_swapon(const char *path, int flags);
long  lp_swapoff(const char *path);

/* /proc 의 한 줄짜리 값을 읽는다. 성공 시 읽은 바이트 수. */
long  proc_read(const char *path, char *buf, size_t size);
/* /proc/meminfo 처럼 "이름:   숫자 kB" 형식에서 값을 찾는다.
 * 못 찾으면 -1. */
long  proc_find_kv(const char *text, const char *key);
long  lp_sync(void);
long  lp_uname(void *buf);
long  lp_getrandom(void *buf, size_t n, unsigned flags);           /* struct utsname 크기 390바이트 */

/* wait 상태 해석 */
#define LP_WIFEXITED(s)    (((s) & 0x7F) == 0)
#define LP_WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define LP_WIFSIGNALED(s)  ((((s) & 0x7F) + 1) >> 1 > 0)
#define LP_WTERMSIG(s)     ((s) & 0x7F)

#endif /* _LP_UNISTD_H */
