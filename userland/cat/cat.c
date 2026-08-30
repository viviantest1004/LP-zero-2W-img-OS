/* cat - 파일 또는 표준입력을 표준출력으로. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 8192

/* fd 하나를 끝까지 복사한다. 성공 0, 실패 1. */
static int copy_fd(int fd, const char *label)
{
    static char buf[BUF_SIZE];

    for (;;) {
        long n = lp_read(fd, buf, sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0) {
            dprintf(STDERR_FILENO, "cat: %s: 읽기 실패 (%ld)\n", label, -n);
            return 1;
        }

        /* write 는 요청보다 적게 쓸 수 있다. 전부 나갈 때까지 반복. */
        long off = 0;
        while (off < n) {
            long w = lp_write(STDOUT_FILENO, buf + off, (size_t)(n - off));
            if (w <= 0) {
                dprintf(STDERR_FILENO, "cat: 쓰기 실패 (%ld)\n", -w);
                return 1;
            }
            off += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return copy_fd(STDIN_FILENO, "stdin");

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            rc |= copy_fd(STDIN_FILENO, "stdin");
            continue;
        }

        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: 열 수 없습니다 (%ld)\n",
                    argv[i], -fd);
            rc = 1;
            continue;
        }
        rc |= copy_fd((int)fd, argv[i]);
        lp_close((int)fd);
    }
    return rc;
}
