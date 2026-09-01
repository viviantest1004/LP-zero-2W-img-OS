/* mv - 파일을 옮기거나 이름을 바꾼다.
 *
 *   mv <원본> <대상>
 *   mv <원본>... <디렉터리>
 *
 * 같은 파일시스템 안에서는 rename 하나로 끝난다 - 내용을 옮기지 않고
 * 디렉터리 항목만 바꾸므로 크기와 무관하게 즉시 끝나고, 중간에 전원이
 * 나가도 반쪽짜리 파일이 남지 않는다.
 *
 * 파일시스템이 다르면(EXDEV) rename 이 안 된다. 예를 들어 RAM 인 /tmp
 * 에서 SD 인 /data 로 옮길 때다. 그때는 복사한 뒤 원본을 지운다.
 * cp 를 부르지 않고 여기서 직접 하는 이유는, 복사가 실패했을 때
 * 원본을 지우면 안 되기 때문이다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 32768
#define EXDEV    18

static int failures = 0;

static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
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

/* 파일시스템을 넘을 때: 복사 -> 확인 -> 원본 삭제. 순서가 중요하다. */
static int move_across(const char *src, const char *dst)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, true);
    if (r < 0) {
        dprintf(STDERR_FILENO, "mv: %s: 읽을 수 없습니다 (%ld)\n", src, -r);
        return 1;
    }
    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        dprintf(STDERR_FILENO,
                "mv: %s: 파일시스템을 넘는 디렉터리 이동은 지원하지 않습니다\n"
                "    (cp -r 로 복사한 뒤 rm -r 로 지우세요)\n", src);
        return 1;
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) {
        dprintf(STDERR_FILENO, "mv: %s: 열 수 없습니다 (%ld)\n", src, -in);
        return 1;
    }
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777);
    if (out < 0) {
        lp_close((int)in);
        dprintf(STDERR_FILENO, "mv: %s: 만들 수 없습니다 (%ld)\n", dst, -out);
        return 1;
    }

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { dprintf(STDERR_FILENO, "mv: 읽기 실패 (%ld)\n", -n); rc = 1; break; }
        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) { dprintf(STDERR_FILENO, "mv: 쓰기 실패 (%ld)\n", -w); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    lp_close((int)in);
    lp_close((int)out);

    if (rc != 0) {
        /* 반쪽만 쓰인 대상을 남기면 안 된다. 원본은 그대로 둔다. */
        lp_unlink(dst);
        return 1;
    }

    lp_chmod(dst, st.mode & 07777);

    long u = lp_unlink(src);
    if (u < 0) {
        dprintf(STDERR_FILENO,
                "mv: %s 는 복사했지만 원본을 지우지 못했습니다 (%ld)\n", dst, -u);
        return 1;
    }
    return 0;
}

static int move_one(const char *src, const char *dst)
{
    long r = lp_rename(src, dst);
    if (r == 0)
        return 0;
    if (r == -EXDEV)
        return move_across(src, dst);

    dprintf(STDERR_FILENO, "mv: %s -> %s: 실패 (%ld)\n", src, dst, -r);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "사용법: mv <원본>... <대상>\n");
        return 2;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);

    if (argc - 1 > 2 && !dst_is_dir) {
        dprintf(STDERR_FILENO, "mv: 원본이 여러 개면 대상은 디렉터리여야 합니다\n");
        return 2;
    }

    for (int i = 1; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                dprintf(STDERR_FILENO, "mv: 경로가 너무 깁니다\n");
                failures = 1;
                continue;
            }
            failures |= move_one(argv[i], full);
        } else {
            failures |= move_one(argv[i], dst);
        }
    }
    return failures;
}
