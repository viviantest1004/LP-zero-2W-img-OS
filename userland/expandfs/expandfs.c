/* expandfs - 데이터 파티션을 SD 카드 끝까지 늘린다.
 *
 * 우리 이미지는 256MB 다. 32GB 카드에 구우면 31.7GB 가 미할당으로 남아
 * 아무것도 하지 않는다. 라즈베리파이 OS 가 첫 부팅에 하는 일을 우리도 한다.
 *
 * 순서:
 *   1) 블록 장치의 실제 크기를 묻는다 (BLKGETSIZE64)
 *   2) MBR 의 2번 항목을 장치 끝까지로 늘려 다시 쓴다
 *   3) 커널에 파티션 테이블을 다시 읽으라고 한다 (BLKRRPART)
 *   4) 마운트한 뒤 ext4 를 늘린다 (EXT4_IOC_RESIZE_FS)
 *
 * resize2fs 를 가져오지 않는 이유: ext4 는 커널이 온라인 확장을 직접
 * 지원한다. ioctl 하나면 커널이 블록 그룹을 추가한다. 유저스페이스
 * 도구는 그 ioctl 을 부르는 게 일의 대부분이다.
 *
 * 안전장치: 늘리기만 한다. 줄이는 경로는 아예 없다.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

#define DEV_DISK      "/dev/mmcblk0"
#define DEV_PART      "/dev/mmcblk0p2"
#define MOUNT_POINT   "/data"
#define PART_INDEX    2            /* 1-기반 */
#define SECTOR_SIZE   512
#define MBR_SIZE      512
#define PART_TABLE_OFF 446
#define PART_ENTRY_LEN 16
#define PART_TYPE_LINUX 0x83

/* 블록 장치 ioctl */
#define BLKRRPART      0x125F      /* _IO(0x12, 95)  파티션 테이블 재읽기 */
#define BLKGETSIZE64   0x80081272  /* _IOR(0x12, 114, size_t)  바이트 크기 */

/* ext4 온라인 확장. _IOW('f', 16, __u64) */
#define EXT4_IOC_RESIZE_FS  0x40086610

/* struct statfs (arm64) 에서 필요한 오프셋 */
#define STATFS_SIZE     120
#define STATFS_BSIZE    8
#define STATFS_BLOCKS   16

/* 늘릴 가치가 있는 최소 크기. 이보다 적게 남으면 건드리지 않는다. */
#define MIN_GROW_MB     16

static long dev_ioctl(const char *path, unsigned long req, void *arg, int flags)
{
    long fd = lp_open(path, flags, 0);
    if (fd < 0)
        return fd;
    long rc = sys_call3(SYS_ioctl, (long)fd, (long)req, (long)arg);
    lp_close((int)fd);
    return rc;
}

/* MBR 의 파티션 항목 하나를 읽는다 */
static bool read_part_entry(const u8 *mbr, int index, u8 *type,
                            u32 *start, u32 *count)
{
    if (index < 1 || index > 4)
        return false;
    const u8 *e = mbr + PART_TABLE_OFF + (index - 1) * PART_ENTRY_LEN;
    *type = e[4];
    memcpy(start, e + 8, 4);
    memcpy(count, e + 12, 4);
    return true;
}

static void write_part_count(u8 *mbr, int index, u32 count)
{
    u8 *e = mbr + PART_TABLE_OFF + (index - 1) * PART_ENTRY_LEN;
    memcpy(e + 12, &count, 4);
    /* 끝 CHS 는 1023 실린더를 넘으면 표현할 수 없다. 관례대로 최대값을
     * 넣는다. 리눅스도 라즈베리파이 부트롬도 LBA 를 보므로 형식상 값이다. */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
}

/* 파티션 테이블을 늘린다. 이미 최대면 false (할 일 없음) */
static bool grow_partition(u64 *new_bytes_out)
{
    u64 dev_bytes = 0;
    long rc = dev_ioctl(DEV_DISK, BLKGETSIZE64, &dev_bytes, O_RDONLY);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: %s 크기를 알 수 없습니다 (%ld)\n",
                DEV_DISK, -rc);
        return false;
    }

    long fd = lp_open(DEV_DISK, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: %s 를 열 수 없습니다 (%ld)\n",
                DEV_DISK, -fd);
        return false;
    }

    u8 mbr[MBR_SIZE];
    if (lp_read((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: MBR 을 읽지 못했습니다\n");
        lp_close((int)fd);
        return false;
    }

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        dprintf(STDERR_FILENO, "expandfs: MBR 시그니처가 없습니다\n");
        lp_close((int)fd);
        return false;
    }

    u8  type; u32 start, count;
    read_part_entry(mbr, PART_INDEX, &type, &start, &count);

    if (type != PART_TYPE_LINUX) {
        dprintf(STDERR_FILENO,
                "expandfs: 파티션 %d 이 리눅스 타입이 아닙니다 (0x%02x)."
                " 안전을 위해 중단합니다\n", PART_INDEX, type);
        lp_close((int)fd);
        return false;
    }

    u64 total_sectors = dev_bytes / SECTOR_SIZE;
    if (total_sectors <= start) {
        dprintf(STDERR_FILENO, "expandfs: 장치가 파티션보다 작습니다\n");
        lp_close((int)fd);
        return false;
    }

    u64 max_count = total_sectors - start;
    if (max_count > 0xFFFFFFFFULL)
        max_count = 0xFFFFFFFFULL;      /* MBR 은 32비트까지 (약 2TB) */

    /* 늘리기만 한다 */
    if (max_count <= count + (MIN_GROW_MB * 1024 * 1024 / SECTOR_SIZE)) {
        printf("expandfs: 이미 최대 크기입니다 (%lu MB)\n",
               (unsigned long)(count / 2048));
        lp_close((int)fd);
        return false;
    }

    printf("expandfs: 파티션 %d 을 %lu MB -> %lu MB 로 늘립니다\n",
           PART_INDEX,
           (unsigned long)(count / 2048),
           (unsigned long)(max_count / 2048));

    write_part_count(mbr, PART_INDEX, (u32)max_count);

    if (lp_lseek((int)fd, 0, 0) < 0 ||
        lp_write((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: MBR 쓰기 실패\n");
        lp_close((int)fd);
        return false;
    }
    lp_sync();
    lp_close((int)fd);

    /* 커널에 새 파티션 테이블을 읽히게 한다.
     * 이 시점에 해당 파티션이 마운트되어 있으면 EBUSY 가 난다. */
    rc = dev_ioctl(DEV_DISK, BLKRRPART, NULL, O_RDONLY);
    if (rc < 0)
        dprintf(STDERR_FILENO,
                "expandfs: 파티션 테이블 재읽기 실패 (%ld)."
                " 재부팅하면 반영됩니다\n", -rc);

    *new_bytes_out = max_count * SECTOR_SIZE;
    return true;
}

/* 마운트된 ext4 를 늘린다 */
static bool grow_filesystem(u64 part_bytes)
{
    u8 st[STATFS_SIZE];
    memset(st, 0, sizeof(st));

    long rc = sys_call2(SYS_statfs, (long)MOUNT_POINT, (long)st);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: statfs 실패 (%ld)\n", -rc);
        return false;
    }

    u64 bsize  = *(u64 *)(st + STATFS_BSIZE);
    u64 blocks = *(u64 *)(st + STATFS_BLOCKS);
    if (bsize == 0) {
        dprintf(STDERR_FILENO, "expandfs: 블록 크기를 알 수 없습니다\n");
        return false;
    }

    u64 want = part_bytes / bsize;
    if (want <= blocks) {
        printf("expandfs: 파일시스템이 이미 파티션을 채웁니다\n");
        return true;
    }

    printf("expandfs: 파일시스템을 %lu MB -> %lu MB 로 늘립니다\n",
           (unsigned long)(blocks * bsize / 1048576),
           (unsigned long)(want * bsize / 1048576));

    /* 커널이 블록 그룹을 추가한다. 마운트된 채로 동작한다(온라인 확장). */
    long fd = lp_open(MOUNT_POINT, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: %s 를 열 수 없습니다 (%ld)\n",
                MOUNT_POINT, -fd);
        return false;
    }

    rc = sys_call3(SYS_ioctl, (long)fd, EXT4_IOC_RESIZE_FS, (long)&want);
    lp_close((int)fd);

    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: 확장 실패 (%ld)\n", -rc);
        return false;
    }

    printf("expandfs: 완료\n");
    return true;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    u64 part_bytes = 0;
    bool grew = grow_partition(&part_bytes);

    /* 파티션을 늘리지 않았어도 파일시스템이 뒤처져 있을 수 있다
     * (예: 지난번에 재읽기가 실패해 재부팅으로 반영된 경우). */
    if (!grew) {
        long fd = lp_open(DEV_PART, O_RDONLY, 0);
        if (fd >= 0) {
            u64 sz = 0;
            sys_call3(SYS_ioctl, (long)fd, BLKGETSIZE64, (long)&sz);
            lp_close((int)fd);
            part_bytes = sz;
        }
    }

    if (part_bytes == 0)
        return 1;

    /* 확장하려면 마운트되어 있어야 한다 */
    bool mounted_here = false;
    if (!lp_is_dir(MOUNT_POINT))
        lp_mkdir(MOUNT_POINT, 0755);

    long rc = lp_mount(DEV_PART, MOUNT_POINT, "ext4", 0, NULL);
    if (rc == 0) {
        mounted_here = true;
    } else if (rc != -16) {         /* -16 = EBUSY, 이미 마운트됨 */
        dprintf(STDERR_FILENO, "expandfs: %s 마운트 실패 (%ld)\n",
                DEV_PART, -rc);
        return 1;
    }

    bool ok = grow_filesystem(part_bytes);

    /* 우리가 마운트했으면 우리가 푼다. rc 스크립트가 다시 마운트한다. */
    if (mounted_here)
        sys_call2(SYS_umount2, (long)MOUNT_POINT, 0);

    return ok ? 0 : 1;
}
