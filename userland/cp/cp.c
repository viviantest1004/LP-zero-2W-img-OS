/* cp - 파일을 복사한다.
 *
 *   cp <원본> <대상>
 *   cp <원본>... <디렉터리>
 *   cp -r <원본>... <대상>      디렉터리째
 *   cp -n <원본> <대상>         대상이 이미 있으면 그대로 둔다 (성공)
 *   cp -q <원본> <대상>         원본이 없어도 오류 메시지를 내지 않는다
 *                               (종료 코드는 여전히 실패)
 *
 * -n 과 -q 는 부팅 스크립트(/etc/rc)를 위한 것이다. 우리 셸에는 if 도
 * test 도 없어서 "있으면 건너뛰고 없으면 만든다"를 표현할 방법이
 * 이것뿐이다.
 *
 * 권한은 원본을 따라간다. 소유자·시각은 옮기지 않는다 - 이 시스템은
 * 사용자가 root 하나뿐이라 의미가 없다.
 *
 * 원본과 대상이 같은 파일이면 거절한다. 그대로 열면 O_TRUNC 로 원본을
 * 먼저 비워버려 내용이 사라진다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE  32768

/* linux_dirent64 의 오프셋. 구조체를 정의하지 않는 이유는
 * 컴파일러의 패딩 규칙에 의존하지 않기 위해서다. (ls.c 와 같다) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19
#define EEXIST    17

#define ENOENT 2

static bool recursive  = false;
static bool no_clobber = false;
static bool quiet      = false;
static int  failures   = 0;

static void oops(const char *what, const char *path, long rc)
{
    if (quiet && rc == -ENOENT)
        return;                 /* 조용히. 실패 표시는 그대로 남긴다. */
    dprintf(STDERR_FILENO, "cp: %s: %s (%ld)\n", path, what, -rc);
    failures = 1;
}

/* 경로의 마지막 요소. "/a/b/c" -> "c", "/a/b/" -> "b" */
static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
}

/* dir 과 name 을 이어 붙인다. 넘치면 false. */
static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlcpy(out, dir, cap);
    if (n >= cap) return false;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
        out[n] = '\0';
    }
    return strlcat(out, name, cap) < cap;
}

static int copy_file(const char *src, const char *dst)
{
    if (no_clobber && lp_exists(dst))
        return 0;               /* 이미 있다. 건드리지 않는 것이 성공이다. */

    lp_stat_t st;
    long r = lp_stat(src, &st, true);
    if (r < 0) {
        oops("읽을 수 없습니다", src, r);
        if (quiet) failures = 1;
        return 1;
    }

    /* 같은 파일을 자기 자신에 복사하면 내용을 잃는다. 미리 막는다.
     * 경로 문자열이 아니라 파일 실체(장치+아이노드)로 비교해야 하지만
     * 우리 stat 은 그 필드를 담지 않는다. 문자열 비교로 흔한 실수만
     * 잡는다. */
    if (strcmp(src, dst) == 0) {
        dprintf(STDERR_FILENO, "cp: %s 와 대상이 같은 파일입니다\n", src);
        failures = 1;
        return 1;
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) { oops("열 수 없습니다", src, in); return 1; }

    /* 원본 권한 그대로 만든다 (실행 파일이 실행 파일로 남게) */
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777);
    if (out < 0) { lp_close((int)in); oops("만들 수 없습니다", dst, out); return 1; }

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { oops("읽기 실패", src, n); rc = 1; break; }

        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) { oops("쓰기 실패", dst, w); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }

    lp_close((int)in);
    lp_close((int)out);

    /* O_CREAT 의 mode 는 umask 에 깎인다. 확실히 맞춘다. */
    if (rc == 0)
        lp_chmod(dst, st.mode & 07777);
    return rc;
}

static int copy_any(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst)
{
    if (!recursive) {
        dprintf(STDERR_FILENO, "cp: %s 는 디렉터리입니다 (-r 을 쓰세요)\n", src);
        failures = 1;
        return 1;
    }

    lp_stat_t st;
    if (lp_stat(src, &st, true) < 0) return 1;

    long r = lp_mkdir(dst, st.mode & 07777);
    if (r < 0 && r != -EEXIST) { oops("만들 수 없습니다", dst, r); return 1; }

    long fd = lp_open(src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { oops("열 수 없습니다", src, fd); return 1; }

    /* 버퍼를 스택에 둔다. static 으로 하면 재귀 호출이 부모의 버퍼를
     * 덮어써서 항목이 조용히 사라진다. 재귀 한 단계당 8KB 다. */
    char dbuf[8192];
    int  rc = 0;

    /* getdents 는 한 번에 다 주지 않는다. 0 이 나올 때까지 돈다. */
    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { oops("읽기 실패", src, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char s[512], t[512];
            if (!join(s, sizeof(s), src, name) ||
                !join(t, sizeof(t), dst, name)) {
                dprintf(STDERR_FILENO, "cp: 경로가 너무 깁니다: %s\n", name);
                rc = 1;
                continue;
            }
            rc |= copy_any(s, t);
        }
    }

    lp_close((int)fd);
    return rc;
}

static int copy_any(const char *src, const char *dst)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, false);        /* 링크는 링크로 본다 */
    if (r < 0) { oops("읽을 수 없습니다", src, r); return 1; }

    if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
        char target[512];
        long n = lp_readlink(src, target, sizeof(target) - 1);
        if (n < 0) { oops("링크를 읽을 수 없습니다", src, n); return 1; }
        target[n] = '\0';
        lp_unlink(dst);                        /* 있으면 지우고 새로 */
        long lr = lp_symlink(target, dst);
        if (lr < 0) { oops("링크를 만들 수 없습니다", dst, lr); return 1; }
        return 0;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR)
        return copy_dir(src, dst);

    return copy_file(src, dst);
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc; first++) {
        const char *a = argv[first];
        if (strcmp(a, "-r") == 0 || strcmp(a, "-R") == 0) recursive  = true;
        else if (strcmp(a, "-n") == 0)                    no_clobber = true;
        else if (strcmp(a, "-q") == 0)                    quiet      = true;
        else break;
    }

    if (argc - first < 2) {
        dprintf(STDERR_FILENO, "사용법: cp [-r] [-n] [-q] <원본>... <대상>\n");
        return 2;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);
    int  nsrc = argc - 1 - first;

    /* 원본이 여러 개면 대상은 반드시 디렉터리여야 한다.
     * 아니면 앞의 것들이 차례로 덮여 마지막 하나만 남는다. */
    if (nsrc > 1 && !dst_is_dir) {
        dprintf(STDERR_FILENO, "cp: 원본이 여러 개면 대상은 디렉터리여야 합니다\n");
        return 2;
    }

    for (int i = first; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                dprintf(STDERR_FILENO, "cp: 경로가 너무 깁니다\n");
                failures = 1;
                continue;
            }
            copy_any(argv[i], full);
        } else {
            copy_any(argv[i], dst);
        }
    }
    return failures;
}
