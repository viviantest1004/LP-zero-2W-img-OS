/* expandfs - grow the data partition to the end of the card.
 *
 * Our image is 256MB. Written to a 32GB card that leaves 31.7GB
 * unallocated and idle. This does what Raspberry Pi OS does on first boot.
 *
 * Steps:
 *   1) ask the block device for its real size (BLKGETSIZE64)
 *   2) rewrite MBR entry 2 to reach the end of the device
 *   3) tell the kernel to re-read the partition table (BLKRRPART)
 *   4) mount it and grow the ext4 (EXT4_IOC_RESIZE_FS)
 *
 * We do not pull in resize2fs because the kernel grows a mounted ext4
 * itself: one ioctl and it adds the block groups. Calling that ioctl is
 * most of what the userspace tool does anyway.
 *
 * Safety: this only ever grows. There is no shrink path at all.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

/* Defaults to the SD card. Pass arguments to use another device, e.g.
 *   expandfs /dev/vda /dev/vda2 */
static const char *dev_disk = "/dev/mmcblk0";
static const char *dev_part = "/dev/mmcblk0p2";
#define DEV_DISK      dev_disk
#define DEV_PART      dev_part
#define MOUNT_POINT   "/data"
#define PART_INDEX    2            /* 1-based */
#define SECTOR_SIZE   512
#define MBR_SIZE      512
#define PART_TABLE_OFF 446
#define PART_ENTRY_LEN 16
#define PART_TYPE_LINUX 0x83

/* Block device ioctls */
#define BLKRRPART      0x125F      /* _IO(0x12, 95)  re-read partition table */
#define BLKGETSIZE64   0x80081272  /* _IOR(0x12, 114, size_t)  size in bytes */

/* ext4 online grow. _IOW('f', 16, __u64) */
#define EXT4_IOC_RESIZE_FS  0x40086610

/* The offsets we need out of struct statfs (arm64) */
#define STATFS_SIZE     120
#define STATFS_BSIZE    8
#define STATFS_BLOCKS   16

/* Not worth growing for less than this. */
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

/* Read one MBR partition entry. */
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
    /* The end CHS cannot express more than 1023 cylinders. Write the
     * conventional maximum: both Linux and the Pi boot ROM read LBA, so
     * this value is a formality. */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
}

/* Grow the partition table. false when it is already at the maximum. */
static bool grow_partition(u64 *new_bytes_out)
{
    u64 dev_bytes = 0;
    long rc = dev_ioctl(DEV_DISK, BLKGETSIZE64, &dev_bytes, O_RDONLY);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot get the size of %s (%ld)\n",
                DEV_DISK, -rc);
        return false;
    }

    long fd = lp_open(DEV_DISK, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot open %s (%ld)\n",
                DEV_DISK, -fd);
        return false;
    }

    u8 mbr[MBR_SIZE];
    if (lp_read((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: could not read the MBR\n");
        lp_close((int)fd);
        return false;
    }

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        dprintf(STDERR_FILENO, "expandfs: no MBR signature\n");
        lp_close((int)fd);
        return false;
    }

    u8  type; u32 start, count;
    read_part_entry(mbr, PART_INDEX, &type, &start, &count);

    if (type != PART_TYPE_LINUX) {
        dprintf(STDERR_FILENO,
                "expandfs: partition %d is not a Linux partition (0x%02x)."
                " Stopping, to be safe\n", PART_INDEX, type);
        lp_close((int)fd);
        return false;
    }

    u64 total_sectors = dev_bytes / SECTOR_SIZE;
    if (total_sectors <= start) {
        dprintf(STDERR_FILENO, "expandfs: the device is smaller than the partition\n");
        lp_close((int)fd);
        return false;
    }

    u64 max_count = total_sectors - start;
    if (max_count > 0xFFFFFFFFULL)
        max_count = 0xFFFFFFFFULL;      /* MBR is 32-bit, about 2TB */

    /* Grow only. */
    if (max_count <= count + (MIN_GROW_MB * 1024 * 1024 / SECTOR_SIZE)) {
        printf("expandfs: already at full size (%lu MB)\n",
               (unsigned long)(count / 2048));
        lp_close((int)fd);
        return false;
    }

    printf("expandfs: growing partition %d from %lu MB to %lu MB\n",
           PART_INDEX,
           (unsigned long)(count / 2048),
           (unsigned long)(max_count / 2048));

    write_part_count(mbr, PART_INDEX, (u32)max_count);

    if (lp_lseek((int)fd, 0, 0) < 0 ||
        lp_write((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: writing the MBR failed\n");
        lp_close((int)fd);
        return false;
    }
    lp_sync();
    lp_close((int)fd);

    /* Make the kernel re-read the partition table. This returns EBUSY if
     * the partition is mounted at this point. */
    rc = dev_ioctl(DEV_DISK, BLKRRPART, NULL, O_RDONLY);
    if (rc < 0)
        dprintf(STDERR_FILENO,
                "expandfs: could not re-read the partition table (%ld)."
                " A reboot will pick it up\n", -rc);

    *new_bytes_out = max_count * SECTOR_SIZE;
    return true;
}

/* Grow a mounted ext4. */
static bool grow_filesystem(u64 part_bytes)
{
    u8 st[STATFS_SIZE];
    memset(st, 0, sizeof(st));

    long rc = sys_call2(SYS_statfs, (long)MOUNT_POINT, (long)st);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: statfs failed (%ld)\n", -rc);
        return false;
    }

    u64 bsize  = *(u64 *)(st + STATFS_BSIZE);
    u64 blocks = *(u64 *)(st + STATFS_BLOCKS);
    if (bsize == 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot determine the block size\n");
        return false;
    }

    u64 want = part_bytes / bsize;
    if (want <= blocks) {
        printf("expandfs: the filesystem already fills the partition\n");
        return true;
    }

    printf("expandfs: growing the filesystem from %lu MB to %lu MB\n",
           (unsigned long)(blocks * bsize / 1048576),
           (unsigned long)(want * bsize / 1048576));

    /* The kernel adds the block groups. This works while mounted. */
    long fd = lp_open(MOUNT_POINT, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot open %s (%ld)\n",
                MOUNT_POINT, -fd);
        return false;
    }

    rc = sys_call3(SYS_ioctl, (long)fd, EXT4_IOC_RESIZE_FS, (long)&want);
    lp_close((int)fd);

    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: grow failed (%ld)\n", -rc);
        return false;
    }

    printf("expandfs: done\n");
    return true;
}

int main(int argc, char **argv)
{
    if (argc >= 3) {
        dev_disk = argv[1];
        dev_part = argv[2];
    } else if (argc == 2) {
        dprintf(STDERR_FILENO,
                "usage: expandfs [disk partition]\n"
                "  e.g.  expandfs /dev/mmcblk0 /dev/mmcblk0p2\n");
        return 2;
    }

    u64 part_bytes = 0;
    bool grew = grow_partition(&part_bytes);

    /* Even when the partition did not change, the filesystem can be behind
     * (say a re-read failed last time and a reboot picked it up). */
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

    /* Growing requires it to be mounted. */
    bool mounted_here = false;
    if (!lp_is_dir(MOUNT_POINT))
        lp_mkdir(MOUNT_POINT, 0755);

    long rc = lp_mount(DEV_PART, MOUNT_POINT, "ext4", 0, NULL);
    if (rc == 0) {
        mounted_here = true;
    } else if (rc != -16) {         /* -16 = EBUSY, already mounted */
        dprintf(STDERR_FILENO, "expandfs: cannot mount %s (%ld)\n",
                DEV_PART, -rc);
        return 1;
    }

    bool ok = grow_filesystem(part_bytes);

    /* If we mounted it, we unmount it. The rc script mounts it again. */
    if (mounted_here)
        sys_call2(SYS_umount2, (long)MOUNT_POINT, 0);

    return ok ? 0 : 1;
}
