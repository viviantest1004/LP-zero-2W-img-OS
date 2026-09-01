/* mkdir - 디렉터리를 만든다.
 *
 *   mkdir <경로>...
 *   mkdir -p <경로>...     중간 경로도 함께 만들고, 이미 있어도 성공
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define EEXIST 17

/* 중간 경로까지 만든다. 이미 있으면 넘어간다. */
static int mkdir_p(const char *path, mode_t mode)
{
    char buf[512];
    if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
        dprintf(STDERR_FILENO, "mkdir: 경로가 너무 깁니다: %s\n", path);
        return 1;
    }

    /* 앞에서부터 '/' 를 만날 때마다 그 지점까지 만든다 */
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        long rc = lp_mkdir(buf, mode);
        if (rc < 0 && rc != -EEXIST) {
            dprintf(STDERR_FILENO, "mkdir: %s: 실패 (%ld)\n", buf, -rc);
            return 1;
        }
        *p = '/';
    }

    long rc = lp_mkdir(buf, mode);
    if (rc < 0 && rc != -EEXIST) {
        dprintf(STDERR_FILENO, "mkdir: %s: 실패 (%ld)\n", buf, -rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool parents = false;
    int  first = 1;

    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        parents = true;
        first = 2;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "사용법: mkdir [-p] <경로>...\n");
        return 2;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (parents) {
            rc |= mkdir_p(argv[i], 0755);
        } else {
            long r = lp_mkdir(argv[i], 0755);
            if (r < 0) {
                dprintf(STDERR_FILENO, "mkdir: %s: 실패 (%ld)\n", argv[i], -r);
                rc = 1;
            }
        }
    }
    return rc;
}
