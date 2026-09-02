/* df - how full each filesystem is.
 *
 *   df [-b]        -b shows blocks in KB rather than MB
 *
 * The list comes from /proc/mounts, minus the virtual filesystems: proc,
 * sysfs and devpts have no size worth reporting and would be four lines
 * of zeroes in front of the two that matter.
 *
 * "free" here is what an ordinary process may still use. ext4 keeps a
 * few percent back for root, so the honest number is smaller than the
 * difference between total and used - and that gap is exactly the
 * reserve that keeps a full disk recoverable.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static bool worth_showing(const char *type)
{
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2",
        "debugfs", "tracefs", "securityfs", "pstore", "bpf", "rootfs", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(type, skip[i]) == 0)
            return false;
    return true;
}

int main(int argc, char **argv)
{
    bool in_kb = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0) in_kb = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: df [-b]\n");
            printf("  -b  show KB instead of MB\n");
            return 0;
        }
    }

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "df: cannot read /proc/mounts\n");
        return 1;
    }

    u64 div = in_kb ? 1024ULL : 1048576ULL;
    printf("%-16s %-12s %9s %9s %9s  use%%\n",
           "filesystem", "on", in_kb ? "total KB" : "total MB",
           "used", "free");

    char line[512];
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        /* device mountpoint type options dump pass */
        char *dev  = line;
        char *sp1  = strchr(line, ' ');
        if (!sp1) continue;
        *sp1 = '\0';
        char *dir  = sp1 + 1;
        char *sp2  = strchr(dir, ' ');
        if (!sp2) continue;
        *sp2 = '\0';
        char *type = sp2 + 1;
        char *sp3  = strchr(type, ' ');
        if (sp3) *sp3 = '\0';

        if (!worth_showing(type))
            continue;

        u64 freeb = 0, totalb = 0;
        if (lp_fs_space(dir, &freeb, &totalb) < 0 || totalb == 0)
            continue;

        u64 used = totalb - freeb;
        printf("%-16s %-12s %9lu %9lu %9lu  %3lu%%\n",
               dev, dir,
               (unsigned long)(totalb / div),
               (unsigned long)(used / div),
               (unsigned long)(freeb / div),
               (unsigned long)(totalb ? used * 100 / totalb : 0));
    }

    lp_close((int)fd);
    return 0;
}
