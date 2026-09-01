/* rm - 파일과 디렉터리를 지운다.
 *
 *   rm <경로>...
 *   rm -r <경로>...      디렉터리째
 *   rm -f <경로>...      없어도 오류로 치지 않음
 *
 * 되돌릴 수 없는 명령이라, 실수를 줄이는 두 가지 안전장치를 넣었다:
 *   · "/" 는 무조건 거절한다
 *   · -r 없이 디렉터리를 지우려 하면 거절한다
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* linux_dirent64 오프셋 (ls.c 와 같다) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define ENOENT 2

static bool recursive = false;
static bool force     = false;
static int  failures  = 0;

static void oops(const char *what, const char *path, long rc)
{
    if (force && rc == -ENOENT)
        return;
    dprintf(STDERR_FILENO, "rm: %s: %s (%ld)\n", path, what, -rc);
    failures = 1;
}

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

static int remove_any(const char *path);

static int remove_dir(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { oops("열 수 없습니다", path, fd); return 1; }

    /* 스택에 둔다. static 이면 재귀가 부모의 목록을 덮어써서
     * 항목이 남은 채로 rmdir 이 실패한다. */
    char dbuf[8192];
    int  rc = 0;

    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { oops("읽기 실패", path, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[512];
            if (!join(child, sizeof(child), path, name)) {
                dprintf(STDERR_FILENO, "rm: 경로가 너무 깁니다: %s\n", name);
                rc = 1;
                continue;
            }
            rc |= remove_any(child);
        }
    }

    lp_close((int)fd);

    /* 안이 비어야 지워진다. 위에서 실패한 것이 있으면 여기서도 실패한다. */
    long r = lp_rmdir(path);
    if (r < 0) { oops("지울 수 없습니다", path, r); return 1; }
    return rc;
}

static int remove_any(const char *path)
{
    lp_stat_t st;
    long r = lp_stat(path, &st, false);      /* 링크는 링크째로 지운다 */
    if (r < 0) { oops("없습니다", path, r); return force ? 0 : 1; }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        if (!recursive) {
            dprintf(STDERR_FILENO, "rm: %s 는 디렉터리입니다 (-r 을 쓰세요)\n", path);
            failures = 1;
            return 1;
        }
        return remove_dir(path);
    }

    r = lp_unlink(path);
    if (r < 0) { oops("지울 수 없습니다", path, r); return force ? 0 : 1; }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc; first++) {
        if (strcmp(argv[first], "-r") == 0 || strcmp(argv[first], "-R") == 0)
            recursive = true;
        else if (strcmp(argv[first], "-f") == 0)
            force = true;
        else if (strcmp(argv[first], "-rf") == 0 || strcmp(argv[first], "-fr") == 0)
            recursive = force = true;
        else
            break;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "사용법: rm [-r] [-f] <경로>...\n");
        return force ? 0 : 2;
    }

    for (int i = first; i < argc; i++) {
        /* 루트를 지우면 되돌릴 방법이 없다. 무조건 막는다.
         * "/" 뿐 아니라 "//" 나 "/." 처럼 결국 루트인 것도 막는다. */
        const char *p = argv[i];
        bool only_root = true;
        for (const char *c = p; *c; c++)
            if (*c != '/' && *c != '.') { only_root = false; break; }
        if (only_root && p[0] == '/') {
            dprintf(STDERR_FILENO, "rm: 루트(%s)는 지울 수 없습니다\n", p);
            failures = 1;
            continue;
        }
        remove_any(p);
    }
    return failures;
}
