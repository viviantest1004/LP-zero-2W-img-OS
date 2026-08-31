/* mount - 파일시스템 마운트/해제.
 *
 *   mount                          마운트 목록 (/proc/mounts)
 *   mount <장치> <위치>            타입 자동 추정
 *   mount -t <타입> <장치> <위치>
 *   mount -o ro ...                읽기 전용
 *   umount <위치>                  (argv[0] 이 umount 면)
 *
 * 타입을 안 주면 아래 목록을 차례로 시도한다. 커널이 맞지 않는 타입에는
 * EINVAL 을 돌려주므로 하나씩 넣어보면 된다. 우리 커널에 들어 있는
 * 파일시스템이 몇 개 안 되므로 금방 끝난다. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

static const char *AUTO_TYPES[] = { "ext4", "vfat", "ext2", NULL };

static int show_mounts(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "mount: /proc/mounts 를 읽을 수 없습니다"
                " (/proc 가 마운트되어 있습니까?)\n");
        return 1;
    }

    char buf[4096];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        lp_write(STDOUT_FILENO, buf, (size_t)n);
    }
    lp_close((int)fd);
    return 0;
}

static int do_umount(const char *target)
{
    long rc = sys_call2(SYS_umount2, (long)target, 0);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "umount: %s: 실패 (%ld)\n", target, -rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* argv[0] 로 umount 여부를 판단한다 (같은 바이너리를 두 이름으로 둔다) */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (strcmp(base, "umount") == 0) {
        if (argc < 2) {
            dprintf(STDERR_FILENO, "사용법: umount <위치>\n");
            return 2;
        }
        return do_umount(argv[1]);
    }

    if (argc == 1)
        return show_mounts();

    const char *type   = NULL;
    unsigned long flags = 0;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            i++;
            if (strstr(argv[i], "ro"))     flags |= MS_RDONLY;
            if (strstr(argv[i], "noexec")) flags |= MS_NOEXEC;
            if (strstr(argv[i], "nosuid")) flags |= MS_NOSUID;
        } else if (nargs < 2) {
            args[nargs++] = argv[i];
        }
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO,
                "사용법: mount [-t 타입] [-o ro] <장치> <위치>\n");
        return 2;
    }

    const char *src = args[0], *dst = args[1];

    if (type) {
        long rc = lp_mount(src, dst, type, flags, NULL);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: %s -> %s (%s): 실패 (%ld)\n",
                    src, dst, type, -rc);
            return 1;
        }
        return 0;
    }

    /* 타입 자동 추정 */
    long last = -1;
    for (int i = 0; AUTO_TYPES[i]; i++) {
        long rc = lp_mount(src, dst, AUTO_TYPES[i], flags, NULL);
        if (rc == 0) {
            printf("mount: %s -> %s (%s)\n", src, dst, AUTO_TYPES[i]);
            return 0;
        }
        last = rc;
    }

    dprintf(STDERR_FILENO, "mount: %s -> %s: 맞는 파일시스템이 없습니다 (%ld)\n",
            src, dst, -last);
    return 1;
}
