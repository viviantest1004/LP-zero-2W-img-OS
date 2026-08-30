/* ls - 디렉터리 목록.
 *
 * getdents64 는 가변 길이 레코드를 버퍼에 연속으로 채워준다:
 *   struct linux_dirent64 {
 *       u64  d_ino;      offset 0
 *       s64  d_off;      offset 8
 *       u16  d_reclen;   offset 16   <- 다음 레코드까지의 바이트
 *       u8   d_type;     offset 18
 *       char d_name[];   offset 19   NUL 로 끝남
 *   };
 * 구조체를 정의하는 대신 오프셋으로 읽는다 - 패딩 규칙에 의존하지 않는다. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF_SIZE       8192
#define DIRENT_RECLEN  16
#define DIRENT_TYPE    18
#define DIRENT_NAME    19

/* d_type 값 */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

static char type_suffix(u8 t)
{
    switch (t) {
    case DT_DIR:  return '/';
    case DT_LNK:  return '@';
    case DT_FIFO: return '|';
    case DT_SOCK: return '=';
    default:      return '\0';
    }
}

static int list_dir(const char *path, bool show_header, bool show_all)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: 열 수 없습니다 (%ld)\n", path, -fd);
        return 1;
    }

    if (show_header)
        printf("%s:\n", path);

    static char buf[BUF_SIZE];
    for (;;) {
        long n = sys_getdents(fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            dprintf(STDERR_FILENO, "ls: %s: 읽기 실패 (%ld)\n", path, -n);
            lp_close((int)fd);
            return 1;
        }

        for (long off = 0; off < n; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;

            if (len == 0)       /* 방어: 무한 루프 방지 */
                break;

            bool hidden = (name[0] == '.');
            if (show_all || !hidden) {
                char suffix = type_suffix(type);
                if (suffix) printf("%s%c\n", name, suffix);
                else        printf("%s\n", name);
            }
            off += len;
        }
    }

    lp_close((int)fd);
    return 0;
}

int main(int argc, char **argv)
{
    bool show_all = false;
    int  first_path = argc;
    int  npaths = 0;

    /* 옵션과 경로를 가른다 */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (char *o = argv[i] + 1; *o; o++) {
                if (*o == 'a') show_all = true;
                else {
                    dprintf(STDERR_FILENO, "ls: 알 수 없는 옵션 -%c\n", *o);
                    return 2;
                }
            }
        } else {
            if (npaths == 0) first_path = i;
            npaths++;
        }
    }

    if (npaths == 0)
        return list_dir(".", false, show_all);

    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        rc |= list_dir(argv[i], npaths > 1, show_all);
    }
    return rc;
}
