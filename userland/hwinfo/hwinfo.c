/* hwinfo - the hardware, device by device.
 *
 *   hwinfo           everything below
 *   hwinfo cpu       one section: machine cpu memory storage network
 *                    pci virtio usb input clock sensors power
 *
 * `info` says how the machine is doing - load, memory in use, disks
 * filling up, addresses. This says what it is: which board or virtual
 * machine, which firmware started it, the processor down to its caches,
 * and every device the kernel found, by name where the name is known and
 * by number where it is not. All of it is read from /proc and /sys; none
 * of it is guessed.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static bool rd(const char *path, char *buf, size_t n)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    long got = lp_read((int)fd, buf, n - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';
    /* device-tree strings end in NUL, sysfs ones in a newline */
    for (long i = 0; i < got; i++)
        if (buf[i] == '\n' || (buf[i] == '\0' && i < got - 1)) { buf[i] = '\0'; break; }
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == ' ')) buf[--got] = '\0';
    return buf[0] != '\0';
}

static bool rdf(char *buf, size_t n, const char *fmt, const char *a, const char *b)
{
    char p[512];
    snprintf(p, sizeof p, fmt, a, b);
    return rd(p, buf, n);
}

static long hexval(const char *s)
{
    return strtol(s, NULL, 16);
}

/* The entries of a directory, sorted, without . and .. */
static int list(const char *dir, char names[][64], int max)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;
    int n = 0;
    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && n < max; ) {
            char *rec = buf + off;
            off += *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            if (name[0] == '.')
                continue;
            strlcpy(names[n++], name, 64);
        }
    }
    lp_close((int)fd);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            strlcpy(t, names[j], 64);
            strlcpy(names[j], names[j - 1], 64);
            strlcpy(names[j - 1], t, 64);
        }
    return n;
}

static void size_str(u64 bytes, char *out, size_t n)
{
    if (bytes >= 1024ULL * 1024 * 1024 * 10)
        snprintf(out, n, "%lluG", (unsigned long long)(bytes >> 30));
    else if (bytes >= 1024ULL * 1024 * 10)
        snprintf(out, n, "%lluM", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, n, "%lluK", (unsigned long long)(bytes >> 10));
}

static void head(const char *t) { printf("\n%s\n", t); }

/* ── machine and firmware ─────────────────────────────────────────── */

static bool is_qemu_virt(void)
{
    char v[128];
    return rd("/proc/device-tree/compatible", v, sizeof v) && strcmp(v, "linux,dummy-virt") == 0;
}

static void machine(void)
{
    char v[256], w[256];
    head("machine");
    if (is_qemu_virt())
        printf("  model        QEMU virt machine   (its device tree says only linux,dummy-virt)\n");
    else {
        if (rd("/proc/device-tree/model", v, sizeof v))
            printf("  model        %s\n", v);
        if (rd("/proc/device-tree/compatible", v, sizeof v))
            printf("  compatible   %s\n", v);
    }
    if (rd("/sys/class/dmi/id/sys_vendor", v, sizeof v)) {
        w[0] = '\0';
        rd("/sys/class/dmi/id/product_name", w, sizeof w);
        printf("  maker        %s %s\n", v, w);
    }
    if (rd("/sys/class/dmi/id/board_name", v, sizeof v))
        printf("  board        %s\n", v);
    if (rd("/sys/class/dmi/id/bios_vendor", v, sizeof v)) {
        w[0] = '\0';
        rd("/sys/class/dmi/id/bios_version", w, sizeof w);
        printf("  bios         %s %s\n", v, w);
    }
    bool efi = lp_access("/sys/firmware/efi", F_OK) == 0;
    bool acpi = lp_access("/sys/firmware/acpi", F_OK) == 0;
    bool dt = lp_access("/proc/device-tree", F_OK) == 0;
    printf("  firmware     %s%s%s\n", efi ? "UEFI" : "no UEFI",
           acpi ? ", ACPI tables" : "", dt ? ", device tree" : "");
    if (is_qemu_virt())
        printf("  virtual      yes - QEMU's virt machine (UTM on a Mac, iPhone or iPad, or QEMU)\n");
    else if (rd("/sys/class/dmi/id/sys_vendor", v, sizeof v) &&
             (strstr(v, "QEMU") || strstr(v, "VMware") || strstr(v, "innotek") || strstr(v, "Xen")))
        printf("  virtual      yes - %s\n", v);
}

/* ── processor ────────────────────────────────────────────────────── */

static char cpuinfo[65536];

static bool cpufield(const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (char *p = cpuinfo; *p; ) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (strncmp(p, key, kl) == 0 && (p[kl] == ' ' || p[kl] == '\t' || p[kl] == ':')) {
            char *c = strchr(p, ':');
            if (c && c < p + len) {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                size_t m = (size_t)(p + len - c);
                if (m >= n) m = n - 1;
                memcpy(out, c, m);
                out[m] = '\0';
                return true;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

static const char *arm_part(long impl, long part)
{
    static const struct { long impl, part; const char *name; } t[] = {
        { 0x41, 0xd03, "ARM Cortex-A53" }, { 0x41, 0xd04, "ARM Cortex-A35" },
        { 0x41, 0xd05, "ARM Cortex-A55" }, { 0x41, 0xd07, "ARM Cortex-A57" },
        { 0x41, 0xd08, "ARM Cortex-A72" }, { 0x41, 0xd09, "ARM Cortex-A73" },
        { 0x41, 0xd0a, "ARM Cortex-A75" }, { 0x41, 0xd0b, "ARM Cortex-A76" },
        { 0x41, 0xd0c, "ARM Neoverse-N1" }, { 0x41, 0xd0d, "ARM Cortex-A77" },
        { 0x41, 0xd41, "ARM Cortex-A78" }, { 0x41, 0xd44, "ARM Cortex-X1" },
        { 0x41, 0xd46, "ARM Cortex-A510" }, { 0x41, 0xd47, "ARM Cortex-A710" },
        { 0x41, 0xd48, "ARM Cortex-X2" }, { 0x41, 0xd4f, "ARM Neoverse-V2" },
        { 0x61, 0x022, "Apple M1 (efficiency)" }, { 0x61, 0x023, "Apple M1 (performance)" },
        { 0x61, 0x032, "Apple M2 (efficiency)" }, { 0x61, 0x033, "Apple M2 (performance)" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].impl == impl && t[i].part == part)
            return t[i].name;
    return NULL;
}

static void cpu(void)
{
    char v[512];
    head("processor");
    if (proc_read("/proc/cpuinfo", cpuinfo, sizeof cpuinfo) <= 0)
        return;
    int n = 0;
    for (char *p = cpuinfo; (p = strstr(p, "processor")) != NULL; p += 9)
        if (p == cpuinfo || p[-1] == '\n') n++;
    if (cpufield("model name", v, sizeof v)) {
        printf("  model        %s\n", v);
    } else {
        char im[32], pa[32];
        if (cpufield("CPU implementer", im, sizeof im) && cpufield("CPU part", pa, sizeof pa)) {
            const char *name = arm_part(hexval(im), hexval(pa));
            char vr[16] = "", rv[16] = "";
            cpufield("CPU variant", vr, sizeof vr);
            cpufield("CPU revision", rv, sizeof rv);
            if (name)
                printf("  model        %s  (r%ldp%s)\n", name, hexval(vr), rv);
            else
                printf("  model        implementer %s, part %s\n", im, pa);
        }
    }
    printf("  cores        %d\n", n ? n : 1);
    if (rd("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", v, sizeof v)) {
        char cur[32] = "";
        rd("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", cur, sizeof cur);
        printf("  clock        %ld MHz now, %ld MHz at most\n",
               strtol(cur, NULL, 10) / 1000, strtol(v, NULL, 10) / 1000);
    } else if (cpufield("cpu MHz", v, sizeof v)) {
        printf("  clock        %s MHz\n", v);
    } else if (cpufield("BogoMIPS", v, sizeof v)) {
        printf("  bogomips     %s   (no clock speed reported here)\n", v);
    }
    char idx[8][64];
    int ni = list("/sys/devices/system/cpu/cpu0/cache", idx, 8);
    for (int i = 0; i < ni; i++) {
        if (strncmp(idx[i], "index", 5) != 0)
            continue;
        char lv[8], ty[24], sz[24];
        if (rdf(lv, sizeof lv, "/sys/devices/system/cpu/cpu0/cache/%s/%s", idx[i], "level") &&
            rdf(ty, sizeof ty, "/sys/devices/system/cpu/cpu0/cache/%s/%s", idx[i], "type") &&
            rdf(sz, sizeof sz, "/sys/devices/system/cpu/cpu0/cache/%s/%s", idx[i], "size"))
            printf("  cache L%s     %s %s\n", lv, sz, ty);
    }
    if (cpufield("flags", v, sizeof v) || cpufield("Features", v, sizeof v))
        printf("  features     %s\n", v);
}

/* ── memory ───────────────────────────────────────────────────────── */

static void memory(void)
{
    char m[4096];
    head("memory");
    if (proc_read("/proc/meminfo", m, sizeof m) <= 0)
        return;
    const char *keys[] = { "MemTotal:", "SwapTotal:" };
    const char *labels[] = { "  ram          ", "  swap         " };
    for (int i = 0; i < 2; i++) {
        char *p = strstr(m, keys[i]);
        if (!p) continue;
        long kb = strtol(p + strlen(keys[i]), NULL, 10);
        printf("%s%ld MB\n", labels[i], kb / 1024);
    }
}

/* ── storage ──────────────────────────────────────────────────────── */

static void storage(void)
{
    char names[32][64], v[128];
    head("storage");
    int n = list("/sys/block", names, 32);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const char *d = names[i];
        if (!strncmp(d, "loop", 4) || !strncmp(d, "ram", 3) || !strncmp(d, "zram", 4))
            continue;
        if (!rdf(v, sizeof v, "/sys/block/%s/%s", d, "size"))
            continue;
        u64 bytes = (u64)strtol(v, NULL, 10) * 512;
        char sz[24];
        size_str(bytes, sz, sizeof sz);
        const char *kind = !strncmp(d, "vd", 2) ? "virtio disk"
                         : !strncmp(d, "mmcblk", 6) ? "SD card / eMMC"
                         : !strncmp(d, "nvme", 4) ? "NVMe"
                         : !strncmp(d, "sd", 2) ? "SATA or USB" : "disk";
        char model[96] = "";
        rdf(model, sizeof model, "/sys/block/%s/device/%s", d, "model");
        if (!model[0]) rdf(model, sizeof model, "/sys/block/%s/device/%s", d, "name");
        char rem[8] = "0";
        rdf(rem, sizeof rem, "/sys/block/%s/%s", d, "removable");
        printf("  /dev/%-9s %7s  %s%s%s%s\n", d, sz, kind, model[0] ? ", " : "", model,
               rem[0] == '1' ? ", removable" : "");
        shown++;
    }
    if (!shown)
        printf("  no disks\n");
}

/* ── network ──────────────────────────────────────────────────────── */

static void network(void)
{
    char names[16][64], v[128];
    head("network");
    int n = list("/sys/class/net", names, 16);
    for (int i = 0; i < n; i++) {
        const char *d = names[i];
        if (!strcmp(d, "lo"))
            continue;
        char mac[32] = "", state[16] = "", drv[96] = "", speed[16] = "";
        rdf(mac, sizeof mac, "/sys/class/net/%s/%s", d, "address");
        rdf(state, sizeof state, "/sys/class/net/%s/%s", d, "operstate");
        rdf(speed, sizeof speed, "/sys/class/net/%s/%s", d, "speed");
        char p[256];
        snprintf(p, sizeof p, "/sys/class/net/%s/device/driver", d);
        long k = lp_readlink(p, v, sizeof v - 1);
        if (k > 0) {
            v[k] = '\0';
            const char *b = strrchr(v, '/');
            strlcpy(drv, b ? b + 1 : v, sizeof drv);
        }
        bool wifi;
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", d);
        wifi = lp_access(p, F_OK) == 0;
        printf("  %-10s %s  %s%s%s%s%s\n", d, mac, wifi ? "wireless" : "wired",
               drv[0] ? ", driver " : "", drv, state[0] ? ", " : "", state);
        if (speed[0] && speed[0] != '-' && strtol(speed, NULL, 10) > 0)
            printf("             %s Mb/s\n", speed);
    }
}

/* ── buses ────────────────────────────────────────────────────────── */

static const char *virtio_name(long id)
{
    switch (id) {
    case 1: return "network";      case 2: return "block (disk)";
    case 3: return "console";      case 4: return "random numbers";
    case 5: return "memory balloon"; case 8: return "SCSI";
    case 9: return "9p shared folder"; case 16: return "GPU";
    case 18: return "input";       case 19: return "vsock";
    case 26: return "virtiofs shared folder"; default: return NULL;
    }
}

static void pci(void)
{
    char names[64][64], v[64], dv[64], cl[64];
    int n = list("/sys/bus/pci/devices", names, 64);
    if (!n)
        return;
    head("pci");
    for (int i = 0; i < n; i++) {
        const char *d = names[i];
        if (!rdf(v, sizeof v, "/sys/bus/pci/devices/%s/%s", d, "vendor") ||
            !rdf(dv, sizeof dv, "/sys/bus/pci/devices/%s/%s", d, "device"))
            continue;
        cl[0] = '\0';
        rdf(cl, sizeof cl, "/sys/bus/pci/devices/%s/%s", d, "class");
        long ven = hexval(v), dev = hexval(dv), cls = hexval(cl) >> 8;
        const char *what = NULL;
        char buf[64];
        if (ven == 0x1af4) {
            long id = dev >= 0x1040 ? dev - 0x1040 : (dev == 0x1000 ? 1 : dev == 0x1001 ? 2 :
                      dev == 0x1003 ? 3 : dev == 0x1005 ? 4 : dev == 0x1002 ? 5 : dev == 0x1009 ? 9 : 0);
            const char *vn = virtio_name(id);
            snprintf(buf, sizeof buf, "virtio %s", vn ? vn : "device");
            what = buf;
        } else if (ven == 0x1b36 && dev == 0x0008) what = "QEMU PCIe host bridge";
        else if (ven == 0x1b36 && dev == 0x000d) what = "QEMU USB 3 controller (xHCI)";
        else if (cls == 0x0200) what = "Ethernet controller";
        else if (cls == 0x0280) what = "network controller";
        else if (cls == 0x0300) what = "display (VGA)";
        else if (cls == 0x0302) what = "3D controller";
        else if (cls == 0x0403) what = "audio";
        else if (cls == 0x0106) what = "SATA controller";
        else if (cls == 0x0108) what = "NVMe controller";
        else if (cls == 0x0c03) what = "USB controller";
        else if (cls == 0x0600) what = "host bridge";
        else if (cls == 0x0604) what = "PCI bridge";
        printf("  %s  %04lx:%04lx  %s\n", d, ven, dev, what ? what : "");
    }
}

static void virtio(void)
{
    char names[32][64], v[32];
    int n = list("/sys/bus/virtio/devices", names, 32);
    if (!n)
        return;
    head("virtio");
    for (int i = 0; i < n; i++) {
        if (!rdf(v, sizeof v, "/sys/bus/virtio/devices/%s/%s", names[i], "device"))
            continue;
        long id = hexval(v);
        const char *vn = virtio_name(id);
        printf("  %-10s %s\n", names[i], vn ? vn : v);
    }
}

static void usb(void)
{
    char names[64][64], pr[96], mf[96], vid[16], pid[16];
    int n = list("/sys/bus/usb/devices", names, 64);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (strchr(names[i], ':'))
            continue;           /* an interface, not a device */
        if (!rdf(vid, sizeof vid, "/sys/bus/usb/devices/%s/%s", names[i], "idVendor"))
            continue;
        if (!shown++) head("usb");
        pid[0] = pr[0] = mf[0] = '\0';
        rdf(pid, sizeof pid, "/sys/bus/usb/devices/%s/%s", names[i], "idProduct");
        rdf(pr, sizeof pr, "/sys/bus/usb/devices/%s/%s", names[i], "product");
        rdf(mf, sizeof mf, "/sys/bus/usb/devices/%s/%s", names[i], "manufacturer");
        printf("  %-8s %s:%s  %s%s%s\n", names[i], vid, pid, mf, mf[0] ? " " : "", pr);
    }
}

static void input(void)
{
    char buf[16384];
    if (proc_read("/proc/bus/input/devices", buf, sizeof buf) <= 0)
        return;
    head("input");
    for (char *p = buf; (p = strstr(p, "N: Name=\"")) != NULL; ) {
        p += 9;
        char *q = strchr(p, '"');
        if (!q) break;
        *q = '\0';
        printf("  %s\n", p);
        p = q + 1;
    }
}

static void clock_(void)
{
    char name[64], d[32], t[32];
    head("clock");
    if (rd("/sys/class/rtc/rtc0/name", name, sizeof name) &&
        rd("/sys/class/rtc/rtc0/date", d, sizeof d) &&
        rd("/sys/class/rtc/rtc0/time", t, sizeof t))
        printf("  rtc0         %s, reads %s %s UTC - the time survives a power cut\n", name, d, t);
    else
        printf("  no hardware clock - the time comes from ntp and what was saved at shutdown\n");
}

static void sensors(void)
{
    char names[16][64], ty[64], tv[32];
    int n = list("/sys/class/thermal", names, 16);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(names[i], "thermal_zone", 12) != 0)
            continue;
        if (!rdf(ty, sizeof ty, "/sys/class/thermal/%s/%s", names[i], "type") ||
            !rdf(tv, sizeof tv, "/sys/class/thermal/%s/%s", names[i], "temp"))
            continue;
        if (!shown++) head("sensors");
        long mc = strtol(tv, NULL, 10);
        printf("  %-20s %ld.%ld C\n", ty, mc / 1000, (mc % 1000) / 100);
    }
}

static void power(void)
{
    char names[16][64], ty[32], cap[16], st[32];
    head("power");
    int n = list("/sys/class/power_supply", names, 16);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (!rdf(ty, sizeof ty, "/sys/class/power_supply/%s/%s", names[i], "type"))
            continue;
        cap[0] = st[0] = '\0';
        rdf(cap, sizeof cap, "/sys/class/power_supply/%s/%s", names[i], "capacity");
        rdf(st, sizeof st, "/sys/class/power_supply/%s/%s", names[i], "status");
        printf("  %-10s %s%s%s%s\n", names[i], ty, cap[0] ? ", " : "", cap,
               cap[0] ? "%" : "");
        if (st[0]) printf("             %s\n", st);
        shown++;
    }
    if (!shown)
        printf("  mains only - no battery (`battery` says the same, in detail when there is one)\n");
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } sec[] = {
        { "machine", machine }, { "cpu", cpu }, { "memory", memory },
        { "storage", storage }, { "network", network }, { "pci", pci },
        { "virtio", virtio }, { "usb", usb }, { "input", input },
        { "clock", clock_ }, { "sensors", sensors }, { "power", power },
    };
    int nsec = (int)(sizeof sec / sizeof sec[0]);
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: hwinfo [SECTION]...\n"
               "The hardware, device by device. Sections:\n ");
        for (int i = 0; i < nsec; i++) printf(" %s", sec[i].name);
        printf("\n`info` shows how it is doing; this shows what it is.\n");
        return 0;
    }
    if (argc < 2) {
        for (int i = 0; i < nsec; i++) sec[i].fn();
        return 0;
    }
    for (int a = 1; a < argc; a++) {
        bool found = false;
        for (int i = 0; i < nsec; i++)
            if (strcmp(argv[a], sec[i].name) == 0) { sec[i].fn(); found = true; }
        if (!found) {
            dprintf(STDERR_FILENO, "hwinfo: no section %s (hwinfo --help)\n", argv[a]);
            return 2;
        }
    }
    return 0;
}
