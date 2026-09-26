/* lp-diskd - the one root process allowed to change disks, and
 * lp-diskctl, the same program used from a terminal.
 *
 *   lp-diskd -d [--socket PATH]       the daemon; /etc/lp/services starts it
 *   lp-diskctl <command> [arg...]     list, check, resize, move, ... (see help)
 *   lp-diskd ctl <command> [arg...]   the same, without the second name
 *
 * The Disks application (desktop/disks, lp-disks) is a partition manager:
 * it shrinks the system partition to make room for a shared one, moves a
 * FAT partition out of the way, formats, labels, checks and repairs. All
 * of that needs root, and the session that runs the application is
 * uid 1000 on purpose. This program is what stands between the two.
 *
 * ── Why a daemon and not a setuid program ──
 *
 * The same reasons lp-privd gives, only sharper: a resize is resize2fs
 * running for minutes over a filesystem it is rewriting, and a move is
 * this program copying gigabytes over the place they came from. A setuid
 * process inherits the caller's resource limits (RLIMIT_FSIZE set low is
 * the classic way to make a root program stop half way through a write),
 * its environment, its descriptors, and can be stopped with ^Z by the
 * person who started it. A daemon started by init has none of that, the
 * caller cannot signal it, and what the caller can influence shrinks to a
 * line of text on a socket. It also serialises the work for free.
 *
 * lp-privd already has a handful of disk verbs (format, mkpart, check);
 * they stay as they are, frozen, and everything a partition manager needs
 * is here instead - GPT as well as MBR, resize, move, flags, SMART, a
 * benchmark, images, /etc/fstab - so that the software centre's daemon
 * does not grow into a disk tool and this one never has to run apt.
 *
 * ── Who may ask ──
 *
 * /run/lp-diskd.sock is mode 0666 and SO_PEERCRED says who connected.
 * Reading - the disk list, SMART, ping, status, /etc/fstab - is open to
 * root and to any real account (uid >= 1000): lsblk shows the same to
 * everybody, and the raw devices it is read from are root's.
 *
 * Changing anything needs root, or membership of group sudo AND the
 * caller's own password within the last five minutes. The password
 * arrives in an "auth" request, is checked with libc's crypt6
 * (lp_shadow_check against /etc/shadow, SHA-512 crypt), and is then
 * remembered for that uid for five minutes - the same bargain sudo and
 * pkexec's auth_admin_keep make. It is never written anywhere. A wrong
 * password makes that uid wait (2 s, doubling, at most 30 s) before the
 * next try is even looked at.
 *
 * Root never needs a password. That is on purpose, and it is what makes
 * the recovery shell work: the owner requires the recovery shell to be
 * root without a password, and lp-diskctl run as root does the work in
 * its own process, with no daemon, no socket and no question asked. In
 * the normal system a person gets root only through sudo, which asks.
 *
 * ── What a request can contain ──
 *
 * One line of printable ASCII, fields separated by TAB, at most 16 KiB.
 * The first field is a verb from a fixed table; every other field is
 * checked against the one shape its position takes before anything
 * else happens: a kernel block device name ("sdb1", never "/dev/sdb1"),
 * a partition GUID, a byte count, a filesystem from a fixed list, a GPT
 * type from a fixed list, a label. Labels may be Korean: a field that
 * starts "x:" is hex-encoded UTF-8 and is checked, after decoding, to be
 * valid UTF-8 without control characters, quotes, slashes or any of the
 * characters Windows refuses in a volume name. None of the alphabets
 * can make a path, and nothing reaches a shell: every tool is started
 * with execve() from an absolute path looked up in fixed directories,
 * with an argv and an environment built here.
 *
 * The only thing a client can hand over that is not text is an open
 * file, for "save an image of this partition" and "restore it": the
 * client opens the file itself, with its own permissions, and passes the
 * descriptor over the socket (SCM_RIGHTS). The daemon checks it is a
 * regular file and never learns, or needs, its name. That is how "never
 * a path from the client" and "save it in my home folder" are both true.
 *
 * ── Plans ──
 *
 * The application queues operations and shows them in plain words; the
 * disk is not touched until Apply. Apply sends the whole queue as one
 * "plan" request, steps separated by a field containing "|". The daemon
 * first plays the plan through on a model of every partition table it
 * touches - overlaps, bounds, alignment, free slots, which filesystems
 * can shrink - and refuses the whole plan if any step would fail. Only
 * then does it start, and after every step it reads the table back from
 * the disk, makes the kernel's view match it, and checks both against
 * what the model said should be there. The first step that fails stops
 * the plan, and the reply ends with "state" lines saying exactly what is
 * on the disk now: which steps finished, which half of the failed step
 * happened ("the filesystem is already 300 GiB but the partition is still
 * 400 GiB - safe"), and what to do next.
 *
 * Partitions are named in a plan by their PARTUUID (the GPT entry's own
 * GUID, or signature-number on MBR), not by sdb2: numbers and kernel
 * names can shift under a plan that deletes and creates, a GUID cannot.
 * A partition that a step earlier in the same plan creates is "@N".
 *
 * ── Telling the kernel ──
 *
 * The partition table is written with sfdisk (util-linux) and the kernel
 * is told with BLKPG - add, delete, resize one partition - rather than
 * with BLKRRPART alone. BLKRRPART re-reads the whole table and fails with
 * EBUSY if any partition of the disk is mounted, which on the laptop is
 * always: shrinking LP-ROOT from the recovery system means editing the
 * disk the recovery system is running from. BLKPG changes only the
 * partitions that changed, and those are never mounted - that is checked
 * first. It is also the only way at all on a kernel without the GPT or
 * MBR parser, which the build host this was tested on is.
 *
 * ── Moving a partition ──
 *
 * Moving is copying every block of the partition to its new place, and
 * when old and new overlap, the copy overwrites the data it is copying.
 * Done in the right direction that is correct; interrupted, it leaves a
 * partition that is neither here nor there. So the copy runs from the
 * end that is safe, in chunks no bigger than half the distance moved, and
 * every so many chunks it flushes the device and writes how far it got
 * to /var/lib/lp-diskd/move.journal (atomically, fsynced). The interval
 * is chosen so that repeating everything after the last journal entry
 * never reads a block the copy has already overwritten. After a power
 * cut, `lp-diskctl resume` (or the app's banner) finishes the move from
 * the journal, and only then is the table pointed at the new place.
 *
 * ── What it refuses no matter who asks ──
 *
 * Any partition that is mounted, is active swap, or has something built
 * on it (device-mapper, LUKS, md). The partition that holds /, /boot,
 * /boot/efi, /usr, /var, /home or /data of the running system: it says
 * to restart into Recovery, where that partition is not in use. A new
 * partition table, or an erase, on a disk with anything in use. Deleting
 * or formatting the ESP or LP-RECOVERY unless the request carries the
 * typed confirmation token the application only sends after the person
 * has typed the name. Secure erase of anything that is not removable.
 *
 * ── Every action is written down ──
 *
 * /var/log/lp-diskd.log gets one line per request - who, what, verdict -
 * one per command run and one for how it ended, and the kernel log gets
 * the same through lp_log. Passwords and passphrases never appear in it.
 *
 * ── The answer ──
 *
 * Lines, ending with exactly one "done ..." or "fail <why> ...". In
 * between: "log <text>", "progress <0-100> <text>", "step <i> <n>
 * <text>" and "stepdone <i>" for plans, "state <text>" before a failed
 * plan's "fail", and TAB-separated "disk"/"part"/"smart"/"fstab"/"bench"
 * records. <why> is denied, auth, invalid, refused, busy, missing,
 * cancelled or failed, so an application can tell "you may not" from
 * "type your password" from "this broke".
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "syscall.h"
#include "crypt6.h"

#define SOCK_PATH     "/run/lp-diskd.sock"
#define LOCK_PATH     "/run/lp-diskd.lock"
#define JOB_FILE      "/run/lp-diskd.job"
#define LOG_PATH      "/var/log/lp-diskd.log"
#define LAST_PATH     "/var/log/lp-diskd.last"
#define STATE_DIR     "/var/lib/lp-diskd"
#define JOURNAL_PATH  STATE_DIR "/move.journal"
#define FSTAB_PATH    "/etc/fstab"
#define LOG_ROTATE    (1024 * 1024)

#define MAX_REQ       16384
#define MAX_FIELDS    160
#define MAX_STEPS     24
#define REQ_TIMEOUT   3000       /* ms a client gets to send its line */
#define AUTH_KEEP_MS  (5 * 60 * 1000)

#define MIB           (1024ull * 1024ull)
#define GIB           (1024ull * MIB)
#define ALIGN_BYTES   MIB        /* every start we choose is on a MiB */

#define AF_UNIX_      1
#define SO_PEERCRED_  17
#define SCM_RIGHTS_   1
#define MSG_DONTWAIT_ 0x40
#define MSG_NOSIGNAL_ 0x4000
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19
#define SIGUSR1_      10
#define EINTR_        4
#define EAGAIN_       11
#define EBUSY_        16
#define LOCK_EX_      2
#define LOCK_NB_      4

/* The system calls libc has no wrapper for, per machine. umask decides
 * the mode of what the tools create; flock serialises the daemon's
 * worker with a root lp-diskctl in another terminal; socketpair is how
 * lp-diskctl runs a request in its own process through exactly the code
 * the daemon uses; recvmsg/sendmsg carry the image file descriptor. */
#if defined(__x86_64__)
#  define SYS_umask_      95
#  define SYS_flock_      73
#  define SYS_socketpair_ 53
#  define SYS_recvmsg_    47
#  define SYS_sendmsg_    46
#elif defined(__aarch64__)
#  define SYS_umask_      166
#  define SYS_flock_      32
#  define SYS_socketpair_ 199
#  define SYS_recvmsg_    212
#  define SYS_sendmsg_    211
#else
#  define SYS_umask_      60
#  define SYS_flock_      143
#  define SYS_socketpair_ 288
#  define SYS_recvmsg_    297
#  define SYS_sendmsg_    296
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Small pieces
 * ═══════════════════════════════════════════════════════════════════ */

static const char *sock_path = SOCK_PATH;

static bool starts(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

/* A value out of sysfs, newline stripped. */
static bool sys_read(const char *path, char *buf, size_t n)
{
    long r = proc_read(path, buf, n);
    if (r <= 0) {
        if (n) buf[0] = '\0';
        return false;
    }
    if ((size_t)r >= n) r = (long)n - 1;
    buf[r] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    /* sysfs pads some values (model names) with trailing spaces. */
    size_t l = strlen(buf);
    while (l > 0 && buf[l - 1] == ' ') buf[--l] = '\0';
    return true;
}

static u64 sys_u64(const char *path)
{
    char v[40];
    if (!sys_read(path, v, sizeof v))
        return 0;
    return (u64)strtoll(v, NULL, 10);
}

/* A decimal number with nothing else in it, and no more digits than a
 * u64 holds. Every size and offset in a request goes through this. */
static bool parse_u64(const char *s, u64 *out)
{
    size_t n = strlen(s);
    if (n == 0 || n > 19)
        return false;
    u64 v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (u64)(s[i] - '0');
    }
    *out = v;
    return true;
}

/* "1.5 GiB". Binary units, because that is what the partitions are cut
 * in and what the application shows; one decimal below 100. */
static void human(u64 bytes, char *out, size_t n)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    u64 whole = bytes, frac = 0;
    while (whole >= 1024 && u < 5) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0 || whole >= 100 || frac == 0)
        snprintf(out, n, "%llu %s", (unsigned long long)whole, unit[u]);
    else
        snprintf(out, n, "%llu.%llu %s", (unsigned long long)whole,
                 (unsigned long long)frac, unit[u]);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex to bytes. false on anything that is not an even run of hex. */
static bool unhex(const char *s, char *out, size_t outn, size_t *len)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 >= outn)
        return false;
    for (size_t i = 0; i < n; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i / 2] = (char)(a * 16 + b);
    }
    out[n / 2] = '\0';
    if (len) *len = n / 2;
    return true;
}

/* Wipe a secret so it does not sit in a freed stack frame. volatile, so
 * the compiler cannot decide the stores are dead. */
static void scrub(void *p, size_t n)
{
    volatile u8 *v = p;
    while (n--) *v++ = 0;
}

/* Only bytes from `allowed` (plus a-z and 0-9, which every alphabet
 * here takes), between 1 and max long, and not starting with `-` - a
 * value that starts with a dash is an option to whatever it is handed
 * to, which is the one injection an argv without a shell still has. */
static bool alphabet_ok(const char *s, const char *allowed, size_t max,
                        bool upper)
{
    size_t n = strlen(s);
    if (n == 0 || n > max || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            continue;
        if (upper && c >= 'A' && c <= 'Z')
            continue;
        if (!strchr(allowed, c))
            return false;
    }
    return true;
}

/* A kernel block device name: sda, sdb1, nvme0n1p2, mmcblk0p1, loop3. */
static bool dev_name_ok(const char *s)
{
    return alphabet_ok(s, "", 31, false) && s[0] >= 'a' && s[0] <= 'z';
}

/* 8-4-4-4-12 hex, either case. */
static bool guid_ok(const char *s)
{
    if (strlen(s) != 36)
        return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (hexval(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

/* MBR's PARTUUID: the disk signature and the slot, "1234abcd-02". */
static bool mbr_partuuid_ok(const char *s)
{
    if (strlen(s) != 11 || s[8] != '-')
        return false;
    for (int i = 0; i < 11; i++)
        if (i != 8 && hexval(s[i]) < 0)
            return false;
    return true;
}

static void upcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static void downcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

/* ── Labels ─────────────────────────────────────────────────────────
 *
 * A label arrives either as itself - letters, digits, space, _ . - - or
 * as "x:" and the hex of its UTF-8, which is how a Korean label crosses
 * a protocol that is ASCII on purpose. "-" alone means "no label".
 *
 * After decoding it has to be well-formed UTF-8 (no overlong forms, no
 * surrogates), with no control characters and none of " \ / : * ? < > |
 * , - quotes and backslashes would break sfdisk's script syntax, and the
 * rest are what Windows refuses in a volume name, so a label that is
 * accepted here is one every system the stick meets will show. */
static bool utf8_clean(const char *s, size_t n, size_t *units16)
{
    size_t u = 0;
    for (size_t i = 0; i < n; ) {
        u8 c = (u8)s[i];
        u32 cp;
        int len;
        if (c < 0x80)      { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; len = 4; }
        else return false;
        if (i + (size_t)len > n)
            return false;
        for (int k = 1; k < len; k++) {
            u8 cc = (u8)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0))
            return false;
        if (cp < 0x80 && strchr("\"\\/:*?<>|,'`$", (int)cp))
            return false;
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)len;
    }
    if (units16) *units16 = u;
    return true;
}

typedef enum { FS_NONE, FS_EXT4, FS_BTRFS, FS_FAT32, FS_EXFAT, FS_NTFS,
               FS_SWAP, FS_LUKS } fstype_t;

static const struct { const char *name; fstype_t fs; } FS_NAMES[] = {
    { "ext4", FS_EXT4 }, { "btrfs", FS_BTRFS }, { "fat32", FS_FAT32 },
    { "exfat", FS_EXFAT }, { "ntfs", FS_NTFS }, { "swap", FS_SWAP },
    { "luks-ext4", FS_LUKS }, { "none", FS_NONE }, { NULL, FS_NONE }
};

static bool fs_parse(const char *s, fstype_t *out)
{
    for (int i = 0; FS_NAMES[i].name; i++)
        if (strcmp(s, FS_NAMES[i].name) == 0) {
            *out = FS_NAMES[i].fs;
            return true;
        }
    return false;
}

static const char *fs_word(fstype_t f)
{
    for (int i = 0; FS_NAMES[i].name; i++)
        if (FS_NAMES[i].fs == f)
            return FS_NAMES[i].name;
    return "none";
}

/* How long a label each filesystem keeps, in bytes (ext4, btrfs, swap:
 * the superblock field is bytes) or UTF-16 units (exFAT, NTFS, GPT
 * names). FAT stores its label in the OEM code page, so it takes ASCII
 * only - a Korean FAT label would come back as question marks on the
 * next machine. */
typedef enum { LBL_BYTES, LBL_UTF16, LBL_ASCII } lblkind_t;

static void label_rule(fstype_t f, size_t *max, lblkind_t *kind)
{
    switch (f) {
    case FS_EXT4:  *max = 16;  *kind = LBL_BYTES; break;
    case FS_LUKS:  *max = 16;  *kind = LBL_BYTES; break;
    case FS_BTRFS: *max = 255; *kind = LBL_BYTES; break;
    case FS_SWAP:  *max = 15;  *kind = LBL_BYTES; break;
    case FS_FAT32: *max = 11;  *kind = LBL_ASCII; break;
    case FS_EXFAT: *max = 11;  *kind = LBL_UTF16; break;
    case FS_NTFS:  *max = 32;  *kind = LBL_UTF16; break;
    default:       *max = 36;  *kind = LBL_UTF16; break;   /* GPT name */
    }
}

/* Decode a label field into out. "-" is the empty label. */
static bool label_decode(const char *field, fstype_t f, bool gpt_name,
                         char *out, size_t outn, char *why, size_t whyn)
{
    size_t max;
    lblkind_t kind;
    if (gpt_name) { max = 36; kind = LBL_UTF16; }
    else label_rule(f, &max, &kind);

    if (strcmp(field, "-") == 0) {
        out[0] = '\0';
        return true;
    }
    size_t len = 0;
    if (starts(field, "x:")) {
        if (!unhex(field + 2, out, outn, &len) || len == 0) {
            snprintf(why, whyn, "a label written as x: must be hex UTF-8");
            return false;
        }
    } else {
        if (!alphabet_ok(field, " _.-", 255, true) || field[0] == ' ') {
            snprintf(why, whyn, "a label is letters, digits, space, _ . -"
                     " (or x: and the hex of its UTF-8)");
            return false;
        }
        strlcpy(out, field, outn);
        len = strlen(out);
    }
    size_t units = 0;
    if (!utf8_clean(out, len, &units) || out[0] == '-' || out[0] == ' ') {
        snprintf(why, whyn, "that label has characters a volume name cannot"
                 " hold");
        return false;
    }
    if (kind == LBL_ASCII) {
        for (size_t i = 0; i < len; i++)
            if ((u8)out[i] >= 0x80) {
                snprintf(why, whyn, "a FAT32 label can only use English"
                         " letters and digits");
                return false;
            }
    }
    size_t used = kind == LBL_UTF16 ? units : len;
    if (used > max) {
        snprintf(why, whyn, "that label is too long for %s: at most %u %s",
                 gpt_name ? "a partition name" : fs_word(f), (unsigned)max,
                 kind == LBL_BYTES ? "bytes" : "characters");
        return false;
    }
    return true;
}

/* ── Partition types ────────────────────────────────────────────────
 *
 * A fixed list, by the names parted and GParted use. The client never
 * sends a GUID or a type byte, so no request can set a type this table
 * does not know. */
typedef struct {
    const char *name;
    const char *gpt;
    u8          mbr;             /* 0 = not on MBR */
    const char *desc;
} ptype_t;

static const ptype_t PTYPES[] = {
    { "linux",   "0FC63DAF-8483-4772-8E79-3D69D8477DE4", 0x83, "Linux filesystem" },
    { "esp",     "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 0xef, "EFI System" },
    { "msdata",  "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 0x07, "Microsoft basic data" },
    { "fat32",   "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 0x0c, "FAT32 (LBA)" },
    { "swap",    "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", 0x82, "Linux swap" },
    { "lvm",     "E6D6D379-F507-44C2-A23C-238F2A3DF928", 0x8e, "Linux LVM" },
    { "raid",    "A19D880F-05FC-4D3B-A006-743F0F84911E", 0xfd, "Linux RAID" },
    { "home",    "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", 0,    "Linux /home" },
    { "root",    "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", 0,    "Linux root (x86-64)" },
    { "luks",    "CA7D7CCB-63ED-4C53-861C-1742536059CC", 0,    "Linux LUKS" },
    { "msres",   "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", 0,    "Microsoft reserved" },
    { "winre",   "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", 0x27, "Windows recovery" },
    { "bios",    "21686148-6449-6E6F-744E-656564454649", 0,    "BIOS boot" },
    { NULL, NULL, 0, NULL }
};

static const ptype_t *ptype_by_name(const char *s)
{
    for (int i = 0; PTYPES[i].name; i++)
        if (strcmp(PTYPES[i].name, s) == 0)
            return &PTYPES[i];
    return NULL;
}

/* The name for a type as found on disk: a GUID, or "0x83". */
static const ptype_t *ptype_find(const char *type)
{
    if (starts(type, "0x")) {
        int v = hexval(type[2]) * 16 + hexval(type[3]);
        for (int i = 0; PTYPES[i].name; i++)
            if (PTYPES[i].mbr == v)
                return &PTYPES[i];
        return NULL;
    }
    for (int i = 0; PTYPES[i].name; i++)
        if (strcmp(PTYPES[i].gpt, type) == 0)
            return &PTYPES[i];
    return NULL;
}

/* The type a filesystem gets when the plan says "auto". */
static const char *auto_type(fstype_t f, bool gpt)
{
    switch (f) {
    case FS_FAT32: return gpt ? "msdata" : "fat32";
    case FS_EXFAT:
    case FS_NTFS:  return "msdata";
    case FS_SWAP:  return "swap";
    default:       return "linux";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * The log
 * ═══════════════════════════════════════════════════════════════════ */

static void stamp(char *out, size_t n)
{
    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);
    snprintf(out, n, "%d-%02d-%02dT%02d:%02d:%02dZ", tm.year, tm.mon,
             tm.day, tm.hour, tm.min, tm.sec);
}

/* One line in /var/log/lp-diskd.log and in the kernel log. Cut at 1MB
 * into a single .1, so years of use cannot fill the disk with it. */
static void audit(const char *text)
{
    char ts[32], line[1400];
    stamp(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%s %s\n", ts, text);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line) {
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    lp_stat_t st;
    if (lp_stat(LOG_PATH, &st, true) == 0 && st.size > LOG_ROTATE)
        lp_rename(LOG_PATH, LOG_PATH ".1");
    long fd = lp_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd >= 0) {
        lp_write((int)fd, line, (size_t)n);
        lp_close((int)fd);
    }
    lp_log("lp-diskd", text);
}

/* ═══════════════════════════════════════════════════════════════════
 * Talking to the client
 * ═══════════════════════════════════════════════════════════════════ */

static int client = -1;          /* the socket of whoever asked */
static int last_fd = -1;         /* /var/log/lp-diskd.last while a job runs */

/* Never blocks and never raises SIGPIPE. A client that stopped reading
 * or went away loses lines; the job does not wait for it - a resize
 * must finish whether or not the window that asked for it is open. */
static void send_raw(const char *s, size_t n)
{
    if (last_fd >= 0)
        lp_write(last_fd, s, n);
    if (client < 0)
        return;
    int tries = 0;
    while (n > 0) {
        long w = lp_sendto(client, s, n, MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, 0);
        if (w == -EAGAIN_ && tries++ < 20) {
            /* The client is slow, not gone: give it a moment, then drop
             * the line rather than stall the job behind it. */
            lp_sleep_ms(5);
            continue;
        }
        if (w <= 0) {
            if (w == -EAGAIN_ || w == -EINTR_)
                return;
            lp_close(client);
            client = -1;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* A reply line. Control characters in `text` become spaces, so every
 * line has exactly the shape the protocol promises. */
static void reply(const char *kind, const char *text)
{
    char line[1200];
    size_t k = strlcpy(line, kind, sizeof line);
    if (text && text[0] && k < sizeof line - 2) {
        line[k++] = ' ';
        for (const char *p = text; *p && k < sizeof line - 2; p++) {
            unsigned char c = (unsigned char)*p;
            line[k++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        }
    }
    line[k++] = '\n';
    send_raw(line, k);
}

static void say(const char *text) { reply("log", text); }

static int  progress_last = -1;
static char job_desc[300];

/* The job file is how a second window, or `lp-diskctl status`, finds
 * out what is running after the window that started it was closed. */
static void job_file(int pct, const char *text)
{
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s\n%d %s\n", job_desc, pct,
                     text ? text : "");
    if (n > 0)
        lp_write_file_atomic(JOB_FILE, buf, (size_t)n);
}

static void progress(int pct, const char *text)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char line[400];
    snprintf(line, sizeof line, "%d %s", pct, text ? text : "");
    reply("progress", line);
    if (pct != progress_last) {
        progress_last = pct;
        job_file(pct, text);
    }
}

/* A TAB-separated record - "disk", "part", "smart" ... Fields are
 * built by the caller; any control byte in them (a tab in a label read
 * off a disk) is turned into '?' so the record keeps its columns. */
static void record(const char *kind, const char *fields)
{
    char out[2400];
    size_t k = strlcpy(out, kind, sizeof out);
    if (k < sizeof out - 2) out[k++] = '\t';
    for (const char *p = fields; *p && k < sizeof out - 2; p++) {
        unsigned char c = (unsigned char)*p;
        out[k++] = (c == '\t') ? '\t' : (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    out[k++] = '\n';
    send_raw(out, k);
}

/* ═══════════════════════════════════════════════════════════════════
 * Block devices, from sysfs
 *
 * Everything goes by the kernel's own name for a device and by
 * /sys/class/block, which knows every partition whatever table
 * described it, and by the partition tables read straight off the disk
 * below - never by udev's /dev/disk links, which the laptop's init does
 * not necessarily have.
 * ═══════════════════════════════════════════════════════════════════ */

static bool blk_exists(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/dev", name);
    return lp_exists(p);
}

static bool blk_is_part(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    return lp_exists(p);
}

static int blk_partno(const char *name)
{
    char p[96], v[16];
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    return sys_read(p, v, sizeof v) ? atoi(v) : 0;
}

/* The disk a partition is on. /sys/class/block/sdb1 is a link to
 * .../block/sdb/sdb1, so the parent is the second-to-last component. */
static bool blk_parent(const char *name, char *out, size_t n)
{
    char p[96], link[512];
    snprintf(p, sizeof p, "/sys/class/block/%s", name);
    long r = lp_readlink(p, link, sizeof link - 1);
    if (r <= 0)
        return false;
    link[r] = '\0';
    char *last = strrchr(link, '/');
    if (!last)
        return false;
    *last = '\0';
    char *prev = strrchr(link, '/');
    strlcpy(out, prev ? prev + 1 : link, n);
    return true;
}

static void blk_disk(const char *name, char *out, size_t n)
{
    if (!blk_is_part(name) || !blk_parent(name, out, n))
        strlcpy(out, name, n);
}

static bool blk_devnum(const char *name, u32 *maj, u32 *min)
{
    char p[96], v[32];
    snprintf(p, sizeof p, "/sys/class/block/%s/dev", name);
    if (!sys_read(p, v, sizeof v))
        return false;
    char *colon = strchr(v, ':');
    if (!colon)
        return false;
    *colon = '\0';
    *maj = (u32)atoi(v);
    *min = (u32)atoi(colon + 1);
    return true;
}

/* Size in bytes. sysfs counts 512-byte units whatever the sector is. */
static u64 blk_bytes(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    return sys_u64(p) * 512;
}

static u32 blk_lss(const char *disk)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", disk);
    u64 v = sys_u64(p);
    return (v == 512 || v == 1024 || v == 2048 || v == 4096) ? (u32)v : 512;
}

/* /dev/<name>, checked to be the block device sysfs says it is. This
 * catches a stale node left by a device that went away and came back
 * numbered differently - which would otherwise send mkfs to the wrong
 * disk - and waits a moment for devtmpfs after a partition appears. */
static bool dev_path(const char *name, char *out, size_t n)
{
    u32 maj, min;
    for (int tries = 0; tries < 30; tries++) {
        if (blk_devnum(name, &maj, &min)) {
            snprintf(out, n, "/dev/%s", name);
            lp_stat_t st;
            if (lp_stat(out, &st, true) == 0 &&
                (st.mode & LP_S_IFMT) == LP_S_IFBLK) {
                u32 smaj = (u32)(((st.rdev >> 8) & 0xfff) | ((st.rdev >> 32) & ~0xfffu));
                u32 smin = (u32)((st.rdev & 0xff) | ((st.rdev >> 12) & ~0xffu));
                if (smaj == maj && smin == min)
                    return true;
            }
        }
        lp_sleep_ms(100);
    }
    return false;
}

static int blk_all(char names[][32], int max)
{
    long fd = lp_open("/sys/class/block", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;
    int n = 0;
    char buf[4096];
    long got;
    while ((got = sys_getdents((int)fd, buf, sizeof buf)) > 0) {
        for (long off = 0; off < got; ) {
            u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
            const char *nm = buf + off + DIRENT_NAME;
            off += len;
            if (nm[0] == '.' || n >= max || strlen(nm) >= 32)
                continue;
            strlcpy(names[n++], nm, 32);
        }
    }
    lp_close((int)fd);
    return n;
}

/* The partitions of one disk that the kernel has now. */
static int blk_parts(const char *disk, char names[][32], int max)
{
    static char all[256][32];
    int na = blk_all(all, 256), n = 0;
    for (int i = 0; i < na && n < max; i++) {
        char parent[32];
        if (blk_is_part(all[i]) && blk_parent(all[i], parent, sizeof parent) &&
            strcmp(parent, disk) == 0)
            strlcpy(names[n++], all[i], 32);
    }
    return n;
}

static bool dir_nonempty(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    char buf[1024];
    long got = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    for (long off = 0; off < got; ) {
        u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
        const char *nm = buf + off + DIRENT_NAME;
        off += len;
        if (nm[0] != '.')
            return true;
    }
    return false;
}

/* Does anything sit on top of this device - device-mapper, md, an open
 * LUKS mapping? Then writing to it pulls the floor from under that. */
static bool blk_has_holders(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/holders", name);
    return dir_nonempty(p);
}

/* The partition name for slot `num` of `disk`: sdb + 1 = sdb1, but
 * nvme0n1 + 1 = nvme0n1p1, mmcblk0 + 1 = mmcblk0p1, loop0 + 1 = loop0p1. */
static void part_name(const char *disk, int num, char *out, size_t n)
{
    size_t l = strlen(disk);
    bool p = l > 0 && disk[l - 1] >= '0' && disk[l - 1] <= '9';
    snprintf(out, n, "%s%s%d", disk, p ? "p" : "", num);
}

/* Which kind of disk this is, for the icon and for "removable". The
 * device link says the bus: .../usb.../ is USB whatever the SCSI layer
 * above it claims; mmcblk is the SD reader; nvme and loop say so in the
 * name. */
static void blk_transport(const char *disk, char *out, size_t n)
{
    char p[96], link[512];
    snprintf(p, sizeof p, "/sys/block/%s", disk);
    long r = lp_readlink(p, link, sizeof link - 1);
    link[r > 0 ? r : 0] = '\0';
    if (starts(disk, "loop"))           strlcpy(out, "loop", n);
    else if (starts(disk, "nvme"))      strlcpy(out, "nvme", n);
    else if (starts(disk, "mmcblk"))    strlcpy(out, "sd", n);
    else if (strstr(link, "/usb"))      strlcpy(out, "usb", n);
    else if (starts(disk, "vd"))        strlcpy(out, "virtio", n);
    else if (starts(disk, "sd") && strstr(link, "/ata")) strlcpy(out, "sata", n);
    else if (starts(disk, "sd"))        strlcpy(out, "scsi", n);
    else if (starts(disk, "sr"))        strlcpy(out, "optical", n);
    else                                strlcpy(out, "other", n);
}

/* Removable means "a person can pull it out": the kernel's own flag,
 * USB, or the SD slot. Loop devices count too, because they are files
 * and nothing is lost that is not a file somebody chose to attach -
 * and because that is what every test of this program runs on. */
static bool blk_removable(const char *disk)
{
    char p[96], tr[16];
    snprintf(p, sizeof p, "/sys/block/%s/removable", disk);
    if (sys_u64(p) == 1)
        return true;
    blk_transport(disk, tr, sizeof tr);
    return !strcmp(tr, "usb") || !strcmp(tr, "sd") || !strcmp(tr, "loop");
}

static void blk_model(const char *disk, char *out, size_t n)
{
    char p[128], v[128], w[64];
    out[0] = '\0';
    snprintf(p, sizeof p, "/sys/block/%s/device/model", disk);
    if (sys_read(p, v, sizeof v) && v[0]) {
        snprintf(p, sizeof p, "/sys/block/%s/device/vendor", disk);
        if (sys_read(p, w, sizeof w) && w[0] && strncmp(v, w, strlen(w)) != 0 &&
            strcmp(w, "ATA") != 0)
            snprintf(out, n, "%s %s", w, v);
        else
            strlcpy(out, v, n);
        return;
    }
    snprintf(p, sizeof p, "/sys/block/%s/device/name", disk);   /* mmc */
    if (sys_read(p, v, sizeof v) && v[0]) {
        snprintf(out, n, "SD card %s", v);
        return;
    }
    if (starts(disk, "loop")) {
        snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", disk);
        if (sys_read(p, v, sizeof v) && v[0]) {
            const char *b = strrchr(v, '/');
            snprintf(out, n, "Loop: %s", b ? b + 1 : v);
            return;
        }
    }
    strlcpy(out, disk, n);
}

/* The disks worth listing: real ones and attached loop devices, not
 * RAM disks, zram, device-mapper or md, and nothing of size zero (an
 * empty card slot, a loop device with no file). */
static bool blk_listable(const char *name)
{
    if (blk_is_part(name))
        return false;
    if (starts(name, "ram") || starts(name, "zram") || starts(name, "dm-") ||
        starts(name, "md") || starts(name, "sr") || starts(name, "fd"))
        return false;
    return blk_bytes(name) > 0;
}

/* ── Mounts ─────────────────────────────────────────────────────────
 *
 * /proc/self/mountinfo, not /proc/mounts, because it gives the device
 * number: a mount of /dev/disk/by-uuid/..., of /dev/root or of a
 * device-mapper name all come back as the same major:minor. */

typedef struct {
    u32  maj, min;
    char point[256];
    char fstype[24];
} mnt_t;

static char   mountinfo[65536];
static mnt_t  mnts[512];
static int    nmnts;

static void mounts_refresh(void)
{
    nmnts = 0;
    long got = proc_read("/proc/self/mountinfo", mountinfo, sizeof mountinfo - 1);
    if (got <= 0)
        return;
    mountinfo[got] = '\0';
    for (char *line = mountinfo; line && *line && nmnts < 512; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *f[6] = {0};
        char *p = line;
        for (int i = 0; i < 6 && p; i++) {
            f[i] = p;
            p = strchr(p, ' ');
            if (p) *p++ = '\0';
        }
        char *dash = p ? strstr(p, " - ") : NULL;
        if (!dash && p && starts(p, "- ")) dash = p - 1;
        if (f[2] && f[4] && dash) {
            char *colon = strchr(f[2], ':');
            if (colon) {
                mnt_t *m = &mnts[nmnts];
                *colon = '\0';
                m->maj = (u32)atoi(f[2]);
                m->min = (u32)atoi(colon + 1);
                size_t k = 0;
                for (char *q = f[4]; *q && k < sizeof m->point - 1; q++) {
                    if (q[0] == '\\' && q[1] >= '0' && q[1] <= '3' &&
                        q[2] >= '0' && q[2] <= '7' && q[3] >= '0' && q[3] <= '7') {
                        m->point[k++] = (char)((q[1] - '0') * 64 + (q[2] - '0') * 8 + (q[3] - '0'));
                        q += 3;
                    } else {
                        m->point[k++] = *q;
                    }
                }
                m->point[k] = '\0';
                char *fs = dash + 3;
                char *sp = strchr(fs, ' ');
                if (sp) *sp = '\0';
                strlcpy(m->fstype, fs, sizeof m->fstype);
                nmnts++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static const char *mounted_at(const char *name)
{
    u32 maj, min;
    if (!blk_devnum(name, &maj, &min))
        return NULL;
    for (int i = 0; i < nmnts; i++)
        if (mnts[i].maj == maj && mnts[i].min == min)
            return mnts[i].point;
    return NULL;
}

static bool name_of(u32 maj, u32 min, char *out, size_t n)
{
    char p[64], link[512];
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", maj, min);
    long r = lp_readlink(p, link, sizeof link - 1);
    if (r <= 0)
        return false;
    link[r] = '\0';
    char *last = strrchr(link, '/');
    strlcpy(out, last ? last + 1 : link, n);
    return true;
}

/* The devices at the bottom of a stack: a partition is itself, dm-0 is
 * whatever is in its slaves/, recursively. */
static void bottoms(const char *name, char out[][32], int *n, int max, int depth)
{
    if (depth > 4 || *n >= max)
        return;
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/slaves", name);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    bool had = false;
    if (fd >= 0) {
        char buf[1024];
        long got = sys_getdents((int)fd, buf, sizeof buf);
        lp_close((int)fd);
        for (long off = 0; off < got; ) {
            u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
            const char *nm = buf + off + DIRENT_NAME;
            off += len;
            if (nm[0] == '.')
                continue;
            had = true;
            bottoms(nm, out, n, max, depth + 1);
        }
    }
    if (had)
        return;
    for (int i = 0; i < *n; i++)
        if (strcmp(out[i], name) == 0)
            return;
    strlcpy(out[(*n)++], name, 32);
}

static const char *const VITAL[] = {
    "/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", "/data", NULL
};

/* Why this device (a partition, or a whole disk used without a table)
 * cannot be written to right now, or NULL. `vital` is set when the
 * reason is that the running system stands on it - the case where the
 * answer is "restart into Recovery" rather than "unmount it". */
static const char *in_use(const char *name, bool *vital, char *why, size_t whyn)
{
    if (vital) *vital = false;
    for (int v = 0; VITAL[v]; v++) {
        for (int i = 0; i < nmnts; i++) {
            if (strcmp(mnts[i].point, VITAL[v]) != 0 || mnts[i].maj == 0)
                continue;
            char dev[32];
            if (!name_of(mnts[i].maj, mnts[i].min, dev, sizeof dev))
                continue;
            char bot[16][32];
            int nb = 0;
            bottoms(dev, bot, &nb, 16, 0);
            for (int k = 0; k < nb; k++) {
                if (strcmp(bot[k], name) == 0) {
                    if (vital) *vital = true;
                    snprintf(why, whyn, "%s holds %s of the running system;"
                             " restart into Recovery to change it", name,
                             VITAL[v]);
                    return "vital";
                }
            }
        }
    }
    const char *at = mounted_at(name);
    if (at) {
        snprintf(why, whyn, "%s is mounted at %s - unmount it first", name, at);
        return "mounted";
    }
    char sw[4096];
    long got = proc_read("/proc/swaps", sw, sizeof sw - 1);
    if (got > 0) {
        sw[got] = '\0';
        for (char *line = sw; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (starts(line, "/dev/")) {
                char dn[32];
                size_t k = 0;
                for (const char *q = line + 5; *q && *q != ' ' && *q != '\t' &&
                     k < sizeof dn - 1; q++)
                    dn[k++] = *q;
                dn[k] = '\0';
                char bot[16][32];
                int nb = 0;
                if (blk_exists(dn))
                    bottoms(dn, bot, &nb, 16, 0);
                for (int i = 0; i < nb; i++)
                    if (strcmp(bot[i], name) == 0) {
                        snprintf(why, whyn, "%s is swap in use", name);
                        return "swap";
                    }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    if (blk_has_holders(name)) {
        snprintf(why, whyn, "%s is in use by another device (an open"
                 " encrypted volume, RAID or LVM)", name);
        return "holder";
    }
    return NULL;
}

/* Anything on this disk in use - the disk itself or any partition. */
static bool disk_busy(const char *disk, bool *vital, char *why, size_t whyn)
{
    if (in_use(disk, vital, why, whyn))
        return true;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    for (int i = 0; i < np; i++)
        if (in_use(parts[i], vital, why, whyn))
            return true;
    return false;
}

/* Is any partition of this disk in use (the running system's disk)? */
static bool disk_is_system(const char *disk)
{
    char why[200];
    bool vital = false;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    for (int i = 0; i < np; i++)
        if (in_use(parts[i], &vital, why, sizeof why) && vital)
            return true;
    return in_use(disk, &vital, why, sizeof why) && vital;
}

/* ═══════════════════════════════════════════════════════════════════
 * Reading the disk
 * ═══════════════════════════════════════════════════════════════════ */

static bool read_at(int fd, u64 off, void *buf, size_t n)
{
    if (lp_lseek(fd, (off_t)off, SEEK_SET) < 0)
        return false;
    size_t got = 0;
    while (got < n) {
        long r = lp_read(fd, (u8 *)buf + got, n - got);
        if (r == -EINTR_)
            continue;
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

static bool write_at(int fd, u64 off, const void *buf, size_t n)
{
    if (lp_lseek(fd, (off_t)off, SEEK_SET) < 0)
        return false;
    size_t put = 0;
    while (put < n) {
        long w = lp_write(fd, (const u8 *)buf + put, n - put);
        if (w == -EINTR_)
            continue;
        if (w <= 0)
            return false;
        put += (size_t)w;
    }
    return true;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* ── What filesystem is on a device ─────────────────────────────────
 *
 * Read-only, a few sectors each. Besides the name, label and UUID it
 * reads how big the filesystem thinks it is and, where the superblock
 * says, how much of it is used: the resize dialog needs both before
 * anything is mounted, and "the filesystem is bigger than its
 * partition" is the check that stands between a shrink and a table
 * that cuts a filesystem short. */

typedef struct {
    char fs[16];               /* ext4 vfat exfat ntfs btrfs swap crypto_LUKS ... */
    char label[96];            /* UTF-8 */
    char uuid[40];
    char state[16];            /* ext: clean / not-clean / errors */
    u64  size;                 /* bytes the filesystem spans; 0 = unknown */
    u64  used;                 /* bytes in use; 0 = unknown */
    bool used_known;
} probe_t;

static void copy_label(const u8 *src, size_t n, char *out, size_t outn)
{
    size_t k = 0;
    for (size_t i = 0; i < n && src[i] && k < outn - 1; i++) {
        u8 c = src[i];
        out[k++] = (c >= 0x20 && c != 0x7f) ? (char)c : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

static size_t put_utf8(u32 c, char *out, size_t k, size_t outn)
{
    if (c < 0x80) {
        if (k + 1 < outn) out[k++] = (char)c;
    } else if (c < 0x800) {
        if (k + 2 < outn) {
            out[k++] = (char)(0xc0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    } else if (c < 0x10000) {
        if (k + 3 < outn) {
            out[k++] = (char)(0xe0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    } else if (k + 4 < outn) {
        out[k++] = (char)(0xf0 | (c >> 18));
        out[k++] = (char)(0x80 | ((c >> 12) & 0x3f));
        out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[k++] = (char)(0x80 | (c & 0x3f));
    }
    return k;
}

/* UTF-16LE (exFAT and NTFS labels, GPT names) to UTF-8. */
static void utf16_to8(const u8 *src, size_t units, char *out, size_t outn)
{
    size_t k = 0;
    for (size_t i = 0; i < units; i++) {
        u32 c = le16(src + 2 * i);
        if (c == 0)
            break;
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
            u32 lo = le16(src + 2 * (i + 1));
            c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
            i++;
        }
        if (c < 0x20) c = '?';
        k = put_utf8(c, out, k, outn);
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

static void guid_str(const u8 *p, char *out)
{
    /* The first three fields are little-endian on disk. */
    static const char h[] = "0123456789ABCDEF";
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        u8 b = p[order[i]];
        out[k++] = h[b >> 4];
        out[k++] = h[b & 15];
    }
    out[k] = '\0';
}

static void hex_uuid(const u8 *p, char *out)
{
    static const char h[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        out[k++] = h[p[i] >> 4];
        out[k++] = h[p[i] & 15];
    }
    out[k] = '\0';
}

static void serial_uuid(u32 v, char *out)
{
    static const char h[] = "0123456789ABCDEF";
    int k = 0;
    for (int i = 7; i >= 0; i--) {
        out[k++] = h[(v >> (i * 4)) & 15];
        if (i == 4) out[k++] = '-';
    }
    out[k] = '\0';
}

static void exfat_label(int fd, const u8 *boot, probe_t *p)
{
    u32 heap = le32(boot + 88), root = le32(boot + 96);
    int bps = boot[108], spc = boot[109];
    if (bps < 9 || bps > 12 || spc > 25 || root < 2)
        return;
    u64 off = ((u64)heap << bps) + ((u64)(root - 2) << (spc + bps));
    static u8 dir[4096];
    if (!read_at(fd, off, dir, sizeof dir))
        return;
    for (int i = 0; i < 4096; i += 32) {
        if (dir[i] == 0x00)
            break;
        if (dir[i] == 0x83) {
            int count = dir[i + 1];
            if (count > 11) count = 11;
            utf16_to8(dir + i + 2, (size_t)count, p->label, sizeof p->label);
            return;
        }
    }
}

static void ntfs_label(int fd, const u8 *boot, probe_t *p)
{
    u32 bps = le16(boot + 0x0b);
    u32 spc = boot[0x0d];
    u64 mft = le64(boot + 0x30);
    s8 cpr = (s8)boot[0x40];
    if (bps < 256 || bps > 4096 || spc == 0)
        return;
    u64 cluster = (u64)bps * spc;
    u32 rec = cpr > 0 ? (u32)(cpr * (s32)cluster) : (1u << (u32)(-cpr));
    if (rec < 512 || rec > 4096)
        return;
    static u8 r[4096];
    if (!read_at(fd, mft * cluster + 3ull * rec, r, rec))
        return;
    if (memcmp(r, "FILE", 4) != 0)
        return;
    /* The update sequence array: put back the last two bytes of every
     * sector, which were swapped for a check value on write. */
    u16 usa = le16(r + 4), usn = le16(r + 6);
    for (u32 s = 1; s < usn && s * bps <= rec && usa + 2u * s + 1 < rec; s++) {
        r[s * bps - 2] = r[usa + 2 * s];
        r[s * bps - 1] = r[usa + 2 * s + 1];
    }
    u32 a = le16(r + 0x14);
    while (a + 24 < rec) {
        u32 type = le32(r + a), len = le32(r + a + 4);
        if (type == 0xffffffffu || len == 0 || a + len > rec)
            break;
        if (type == 0x60 && r[a + 8] == 0) {
            u32 vlen = le32(r + a + 0x10);
            u32 voff = le16(r + a + 0x14);
            if (a + voff + vlen <= rec)
                utf16_to8(r + a + voff, vlen / 2, p->label, sizeof p->label);
            return;
        }
        a += len;
    }
}

static bool probe_fd(int fd, probe_t *p)
{
    memset(p, 0, sizeof *p);
    static u8 h[4096];
    memset(h, 0, sizeof h);
    if (!read_at(fd, 0, h, sizeof h))
        return false;

    if (le16(h + 1024 + 56) == 0xEF53) {
        const u8 *sb = h + 1024;
        u32 compat = le32(sb + 92), incompat = le32(sb + 96);
        strlcpy(p->fs, (incompat & 0x2c0) ? "ext4" :
                       (compat & 0x4) ? "ext3" : "ext2", sizeof p->fs);
        copy_label(sb + 120, 16, p->label, sizeof p->label);
        hex_uuid(sb + 104, p->uuid);
        u16 state = le16(sb + 58);
        u32 errors = le32(sb + 0x194);
        strlcpy(p->state, (state & 2) || errors ? "errors" :
                          (state & 1) ? "clean" : "not-clean", sizeof p->state);
        u64 bsize = 1024ull << le32(sb + 24);
        bool b64 = (incompat & 0x80) != 0;
        u64 blocks = le32(sb + 4) | (b64 ? (u64)le32(sb + 0x150) << 32 : 0);
        u64 freeb  = le32(sb + 12) | (b64 ? (u64)le32(sb + 0x158) << 32 : 0);
        p->size = blocks * bsize;
        if (freeb <= blocks) {
            p->used = (blocks - freeb) * bsize;
            p->used_known = true;
        }
    } else if (memcmp(h + 3, "EXFAT   ", 8) == 0) {
        strlcpy(p->fs, "exfat", sizeof p->fs);
        serial_uuid(le32(h + 100), p->uuid);
        int bps = h[108];
        if (bps >= 9 && bps <= 12)
            p->size = le64(h + 72) << bps;
        /* PercentInUse: the formatter's own count, 0xFF when unknown. */
        if (h[112] <= 100 && p->size) {
            p->used = p->size / 100 * h[112];
            p->used_known = true;
        }
        exfat_label(fd, h, p);
    } else if (memcmp(h + 3, "NTFS    ", 8) == 0) {
        strlcpy(p->fs, "ntfs", sizeof p->fs);
        u64 serial = le64(h + 0x48);
        static const char hx[] = "0123456789ABCDEF";
        for (int i = 0; i < 16; i++)
            p->uuid[i] = hx[(serial >> ((15 - i) * 4)) & 15];
        p->uuid[16] = '\0';
        /* Total sectors excludes the backup boot sector at the end. */
        p->size = (le64(h + 0x28) + 1) * le16(h + 0x0b);
        ntfs_label(fd, h, p);
    } else if (h[510] == 0x55 && h[511] == 0xAA &&
               (memcmp(h + 82, "FAT32", 5) == 0 || memcmp(h + 54, "FAT1", 4) == 0)) {
        bool f32 = memcmp(h + 82, "FAT32", 5) == 0;
        strlcpy(p->fs, "vfat", sizeof p->fs);
        copy_label(h + (f32 ? 71 : 43), 11, p->label, sizeof p->label);
        if (strcmp(p->label, "NO NAME") == 0)
            p->label[0] = '\0';
        serial_uuid(le32(h + (f32 ? 67 : 39)), p->uuid);
        u32 bps = le16(h + 11);
        u32 tot = le16(h + 19) ? le16(h + 19) : le32(h + 32);
        p->size = (u64)tot * bps;
        if (f32) {
            /* FSInfo's free-cluster count is a hint the driver keeps up
             * to date on a clean unmount; 0xFFFFFFFF means "not known". */
            static u8 fsi[512];
            u32 spc = h[13];
            u32 rsv = le16(h + 14), nfat = h[16], fatsz = le32(h + 36);
            u64 clusters = spc ? (tot - rsv - nfat * fatsz) / spc : 0;
            if (read_at(fd, (u64)le16(h + 48) * bps, fsi, 512) &&
                le32(fsi) == 0x41615252 && le32(fsi + 488) != 0xffffffffu &&
                le32(fsi + 488) <= clusters) {
                p->used = (clusters - le32(fsi + 488)) * spc * bps;
                p->used_known = true;
            }
        }
    } else if (memcmp(h + 4086, "SWAPSPACE2", 10) == 0) {
        strlcpy(p->fs, "swap", sizeof p->fs);
        copy_label(h + 1024 + 28, 16, p->label, sizeof p->label);
        hex_uuid(h + 1024 + 12, p->uuid);
        p->size = ((u64)le32(h + 1024 + 4) + 1) * 4096;
    } else if (memcmp(h, "LUKS\xba\xbe", 6) == 0) {
        strlcpy(p->fs, "crypto_LUKS", sizeof p->fs);
        copy_label(h + 168, 36, p->uuid, sizeof p->uuid);
        if (le16(h + 6) == 2)
            copy_label(h + 24, 48, p->label, sizeof p->label);
    } else {
        static u8 x[4096];
        if (read_at(fd, 0x10000, x, 4096) && memcmp(x + 0x40, "_BHRfS_M", 8) == 0) {
            strlcpy(p->fs, "btrfs", sizeof p->fs);
            copy_label(x + 0x12b, 256, p->label, sizeof p->label);
            hex_uuid(x + 0x20, p->uuid);
            p->size = le64(x + 0x70);
            p->used = le64(x + 0x78);
            p->used_known = true;
        } else if (read_at(fd, 0x8001, x, 64) && memcmp(x, "CD001", 5) == 0) {
            strlcpy(p->fs, "iso9660", sizeof p->fs);
            copy_label(x + 39, 32, p->label, sizeof p->label);
        }
    }
    return true;
}

static bool probe_dev(const char *name, probe_t *p)
{
    memset(p, 0, sizeof *p);
    char dev[64];
    if (!dev_path(name, dev, sizeof dev))
        return false;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    bool ok = probe_fd((int)fd, p);
    lp_close((int)fd);
    return ok;
}

static fstype_t fs_of_probe(const probe_t *p)
{
    if (starts(p->fs, "ext"))            return FS_EXT4;
    if (!strcmp(p->fs, "vfat"))          return FS_FAT32;
    if (!strcmp(p->fs, "exfat"))         return FS_EXFAT;
    if (!strcmp(p->fs, "ntfs"))          return FS_NTFS;
    if (!strcmp(p->fs, "btrfs"))         return FS_BTRFS;
    if (!strcmp(p->fs, "swap"))          return FS_SWAP;
    if (!strcmp(p->fs, "crypto_LUKS"))   return FS_LUKS;
    return FS_NONE;
}

/* ── Partition tables ───────────────────────────────────────────────
 *
 * Read straight off the disk, both kinds. This is the source of truth
 * for every check and every "after" in a plan; sysfs only says what the
 * kernel currently believes, which is what kernel_sync() corrects. */

#define MAX_PARTS 128

typedef struct {
    int  num;                  /* the kernel's partition number */
    u64  start, size;          /* logical sectors */
    char type[40];             /* GPT GUID, upper case, or "0x83" */
    char uuid[40];             /* GPT unique GUID (upper), or "sig-nn" */
    char name[112];            /* GPT name, UTF-8 */
    u64  attrs;                /* GPT attribute bits; MBR: bit 7 = active */
    bool logical;              /* MBR logical partition (read, not edited) */
} pent_t;

typedef struct {
    char   disk[32];
    char   kind[8];            /* gpt, mbr, none */
    char   id[40];             /* disk GUID, or the MBR signature */
    u32    lss;                /* logical sector size */
    u64    nsect;              /* disk size in logical sectors */
    u64    first, last;        /* usable sectors, inclusive */
    bool   has_extended;
    char   err[160];           /* what is wrong with the table, if anything */
    int    n;
    pent_t p[MAX_PARTS];
} table_t;

static u32 crc32_tab[256];

static u32 crc32(const u8 *p, size_t n)
{
    if (!crc32_tab[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc32_tab[i] = c;
        }
    }
    u32 c = 0xffffffffu;
    for (size_t i = 0; i < n; i++)
        c = crc32_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static bool gpt_read(int fd, table_t *t, u64 lba, bool primary)
{
    static u8 hdr[4096];
    if (!read_at(fd, lba * t->lss, hdr, t->lss) || memcmp(hdr, "EFI PART", 8) != 0)
        return false;
    u32 hsize = le32(hdr + 12);
    if (hsize < 92 || hsize > t->lss)
        return false;
    u32 want = le32(hdr + 16);
    static u8 tmp[4096];
    memcpy(tmp, hdr, hsize);
    memset(tmp + 16, 0, 4);
    if (crc32(tmp, hsize) != want) {
        snprintf(t->err, sizeof t->err, "the %s GPT header is damaged (CRC)",
                 primary ? "primary" : "backup");
        return false;
    }
    t->first = le64(hdr + 40);
    t->last = le64(hdr + 48);
    guid_str(hdr + 56, t->id);
    u64 elba = le64(hdr + 72);
    u32 count = le32(hdr + 80), esize = le32(hdr + 84);
    if (count > MAX_PARTS * 4 || esize < 128 || esize > 1024)
        return false;
    size_t bytes = (size_t)count * esize;
    u8 *ents = malloc(bytes + 1);
    if (!ents)
        return false;
    if (!read_at(fd, elba * t->lss, ents, bytes)) {
        free(ents);
        return false;
    }
    if (crc32(ents, bytes) != le32(hdr + 88)) {
        snprintf(t->err, sizeof t->err, "the %s GPT entry array is damaged (CRC)",
                 primary ? "primary" : "backup");
        free(ents);
        return false;
    }
    t->n = 0;
    for (u32 i = 0; i < count && t->n < MAX_PARTS; i++) {
        const u8 *e = ents + (size_t)i * esize;
        static const u8 zero[16];
        if (memcmp(e, zero, 16) == 0)
            continue;
        pent_t *p = &t->p[t->n++];
        memset(p, 0, sizeof *p);
        p->num = (int)i + 1;
        guid_str(e, p->type);
        guid_str(e + 16, p->uuid);
        p->start = le64(e + 32);
        p->size = le64(e + 40) - p->start + 1;
        p->attrs = le64(e + 48);
        utf16_to8(e + 56, 36, p->name, sizeof p->name);
    }
    free(ents);
    return true;
}

static void mbr_entry(pent_t *p, const u8 *e, int num, u64 base, u32 sig)
{
    memset(p, 0, sizeof *p);
    p->num = num;
    snprintf(p->type, sizeof p->type, "0x%02x", e[4]);
    p->start = base + le32(e + 8);
    p->size = le32(e + 12);
    p->attrs = e[0] & 0x80;
    snprintf(p->uuid, sizeof p->uuid, "%08x-%02x", sig, num);
}

static bool table_read(const char *disk, table_t *t)
{
    memset(t, 0, sizeof *t);
    strlcpy(t->disk, disk, sizeof t->disk);
    strlcpy(t->kind, "none", sizeof t->kind);
    t->lss = blk_lss(disk);
    t->nsect = blk_bytes(disk) / t->lss;
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev))
        return false;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;

    static u8 mbr[512];
    bool have_mbr = read_at((int)fd, 0, mbr, 512) && mbr[510] == 0x55 && mbr[511] == 0xAA;
    bool protective = false;
    for (int s = 0; have_mbr && s < 4; s++)
        if (mbr[446 + s * 16 + 4] == 0xee)
            protective = true;

    if (gpt_read((int)fd, t, 1, true)) {
        strlcpy(t->kind, "gpt", sizeof t->kind);
    } else if (protective) {
        /* Primary damaged: the backup at the last sector is what sgdisk
         * and parted fall back to, and it is what we show - with the
         * damage said out loud. */
        char err[160];
        strlcpy(err, t->err, sizeof err);
        if (gpt_read((int)fd, t, t->nsect - 1, false)) {
            strlcpy(t->kind, "gpt", sizeof t->kind);
            snprintf(t->err, sizeof t->err, "%s; showing the backup copy",
                     err[0] ? err : "the primary GPT is missing");
        } else {
            strlcpy(t->kind, "gpt", sizeof t->kind);
            if (!t->err[0])
                strlcpy(t->err, "GPT is damaged beyond reading", sizeof t->err);
        }
    } else if (have_mbr) {
        /* An MBR, or a filesystem boot sector (FAT and NTFS end in 55AA
         * too). A table has sane entries and no filesystem signature. */
        bool fsboot = memcmp(mbr + 3, "NTFS", 4) == 0 || memcmp(mbr + 3, "EXFAT", 5) == 0 ||
                      memcmp(mbr + 82, "FAT32", 5) == 0 || memcmp(mbr + 54, "FAT", 3) == 0;
        bool sane = true;
        for (int s = 0; s < 4; s++) {
            u8 st = mbr[446 + s * 16];
            if (st != 0 && st != 0x80) sane = false;
        }
        if (sane && !fsboot) {
            strlcpy(t->kind, "mbr", sizeof t->kind);
            u32 sig = le32(mbr + 440);
            snprintf(t->id, sizeof t->id, "%08x", sig);
            t->first = 2048 * 512 / t->lss;
            if (t->first < 1) t->first = 1;
            t->last = t->nsect - 1;
            for (int s = 0; s < 4; s++) {
                const u8 *e = mbr + 446 + s * 16;
                if (e[4] == 0 || le32(e + 12) == 0)
                    continue;
                if (e[4] == 0x05 || e[4] == 0x0f || e[4] == 0x85) {
                    t->has_extended = true;
                    /* Walk the chain of logical partitions: each EBR
                     * describes one, and links to the next relative to
                     * the start of the extended partition. */
                    u64 ext = le32(e + 8), next = ext;
                    int num = 5;
                    static u8 ebr[512];
                    for (int guard = 0; guard < 64 && t->n < MAX_PARTS; guard++) {
                        if (!read_at((int)fd, next * t->lss, ebr, 512) ||
                            ebr[510] != 0x55 || ebr[511] != 0xAA)
                            break;
                        const u8 *l = ebr + 446;
                        if (l[4] && le32(l + 12)) {
                            mbr_entry(&t->p[t->n], l, num++, next, sig);
                            t->p[t->n++].logical = true;
                        }
                        const u8 *link = ebr + 446 + 16;
                        if (!le32(link + 8))
                            break;
                        next = ext + le32(link + 8);
                    }
                    continue;
                }
                mbr_entry(&t->p[t->n++], e, s + 1, 0, sig);
            }
        }
    }
    lp_close((int)fd);

    /* In number order: GPT entries are, MBR logicals come after. */
    for (int i = 1; i < t->n; i++)
        for (int j = i; j > 0 && t->p[j - 1].num > t->p[j].num; j--) {
            pent_t x = t->p[j]; t->p[j] = t->p[j - 1]; t->p[j - 1] = x;
        }
    return true;
}

static pent_t *table_num(table_t *t, int num)
{
    for (int i = 0; i < t->n; i++)
        if (t->p[i].num == num)
            return &t->p[i];
    return NULL;
}

static pent_t *table_uuid(table_t *t, const char *uuid)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->p[i].uuid, uuid) == 0)
            return &t->p[i];
    return NULL;
}

/* The disk and table entry a PARTUUID names, searched over every disk.
 * GPT GUIDs are compared upper case, MBR ones lower, as they are read. */
static bool find_partuuid(const char *uuid_in, char *disk, size_t dn, int *num)
{
    char uuid[40];
    strlcpy(uuid, uuid_in, sizeof uuid);
    if (guid_ok(uuid)) upcase(uuid); else downcase(uuid);
    static char names[256][32];
    int n = blk_all(names, 256);
    static table_t t;
    for (int i = 0; i < n; i++) {
        if (!blk_listable(names[i]))
            continue;
        if (!table_read(names[i], &t))
            continue;
        pent_t *p = table_uuid(&t, uuid);
        if (p) {
            strlcpy(disk, names[i], dn);
            *num = p->num;
            return true;
        }
    }
    return false;
}

/* ESP and LP-RECOVERY are what the machine boots and recovers with.
 * Deleting or formatting them wants a token the person typed. */
static const char *protected_tag(const table_t *t, const pent_t *p)
{
    if (!strcmp(p->type, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") ||
        (!strcmp(t->kind, "mbr") && !strcmp(p->type, "0xef")))
        return "ESP";
    if (!strcmp(p->name, "LP-RECOVERY"))
        return "LP-RECOVERY";
    return NULL;
}

/* The flags GParted shows, from type and attributes together. */
static void flags_str(const table_t *t, const pent_t *p, char *out, size_t n)
{
    out[0] = '\0';
    bool gpt = !strcmp(t->kind, "gpt");
    const ptype_t *ty = ptype_find(p->type);
    const char *nm = ty ? ty->name : "";
    #define ADD(s) do { if (out[0]) strlcat(out, ",", n); strlcat(out, s, n); } while (0)
    if (!strcmp(nm, "esp"))   { ADD("esp"); ADD("boot"); }
    if (!gpt && (p->attrs & 0x80) && strcmp(nm, "esp")) ADD("boot");
    if (!strcmp(nm, "msdata") || (!gpt && !strcmp(nm, "fat32"))) ADD("msftdata");
    if (!strcmp(nm, "lvm"))   ADD("lvm");
    if (!strcmp(nm, "raid"))  ADD("raid");
    if (!strcmp(nm, "swap"))  ADD("swap");
    if (!strcmp(nm, "bios"))  ADD("bios_grub");
    if (!strcmp(nm, "msres")) ADD("msftres");
    if (!strcmp(nm, "winre")) ADD("diag");
    if (gpt && (p->attrs & (1ull << 0)))  ADD("required");
    if (gpt && (p->attrs & (1ull << 2)))  ADD("legacy_boot");
    if (gpt && (p->attrs & (1ull << 60))) ADD("readonly");
    if (gpt && (p->attrs & (1ull << 62))) ADD("hidden");
    if (gpt && (p->attrs & (1ull << 63))) ADD("no_automount");
    /* MBR hides a partition by setting 0x10 in its type: 0x11, 0x14,
     * 0x16, 0x17, 0x1b, 0x1c, 0x1e are FAT and NTFS, hidden. */
    if (!gpt && p->type[2] == '1' && p->type[3] && strchr("1467bce", p->type[3]))
        ADD("hidden");
    #undef ADD
}

/* ═══════════════════════════════════════════════════════════════════
 * Telling the kernel
 * ═══════════════════════════════════════════════════════════════════ */

#define BLKRRPART_     0x125f
#define BLKFLSBUF_     0x1261
#define BLKDISCARD_    0x1277
#define BLKPG_         0x1269
#define BLKPG_ADD_     1
#define BLKPG_DEL_     2
#define BLKPG_RESIZE_  3

typedef struct {
    long long start, length;
    int  pno;
    char devname[64], volname[64];
} blkpg_part_t;

typedef struct {
    int   op, flags, datalen;
    void *data;
} blkpg_arg_t;

static long blkpg(int fd, int op, int pno, u64 start_b, u64 len_b)
{
    blkpg_part_t part;
    memset(&part, 0, sizeof part);
    part.pno = pno;
    part.start = (long long)start_b;
    part.length = (long long)len_b;
    blkpg_arg_t arg = { op, 0, (int)sizeof part, &part };
    return lp_ioctl(fd, BLKPG_, &arg);
}

/* Make the kernel's partitions of `disk` match the table on the disk.
 *
 * BLKRRPART first when nothing on the disk is in use - that is the
 * ordinary way, and it makes udev see a clean change. Then, whatever it
 * did or did not do, compare partition by partition and fix the rest
 * with BLKPG: a disk with a mounted partition refuses BLKRRPART
 * outright, and a kernel without the parser for this table type accepts
 * it and adds nothing. A partition that is in use is never touched;
 * if the table and the kernel disagree about one of those, the plan
 * checks should already have stopped us, and it is reported. */
static bool kernel_sync(const char *disk, char *why, size_t whyn)
{
    static table_t t;
    if (!table_read(disk, &t)) {
        snprintf(why, whyn, "cannot read the partition table of %s", disk);
        return false;
    }
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev)) {
        snprintf(why, whyn, "%s disappeared", disk);
        return false;
    }
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(why, whyn, "cannot open %s", dev);
        return false;
    }
    lp_sync();
    lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    mounts_refresh();
    bool vital;
    char tmp[200];
    if (!disk_busy(disk, &vital, tmp, sizeof tmp))
        lp_ioctl((int)fd, BLKRRPART_, NULL);

    u64 f = t.lss / 512;                 /* table sectors -> sysfs units */
    bool ok = true;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    /* Remove or shrink what the table no longer has. */
    for (int i = 0; i < np; i++) {
        int num = blk_partno(parts[i]);
        char p[96];
        snprintf(p, sizeof p, "/sys/class/block/%s/start", parts[i]);
        u64 kstart = sys_u64(p);
        snprintf(p, sizeof p, "/sys/class/block/%s/size", parts[i]);
        u64 ksize = sys_u64(p);
        pent_t *e = table_num(&t, num);
        bool same = e && e->start * f == kstart && e->size * f == ksize;
        if (same)
            continue;
        /* An MBR extended container shows as a 1 KiB partition in the
         * kernel; the table has it as a whole range. Leave it. */
        if (e && e->start * f == kstart && ksize == 2)
            continue;
        if (in_use(parts[i], &vital, tmp, sizeof tmp)) {
            snprintf(why, whyn, "the kernel still uses the old %s (%s)", parts[i], tmp);
            ok = false;
            continue;
        }
        long r;
        if (e && e->start * f == kstart)
            r = blkpg((int)fd, BLKPG_RESIZE_, num, e->start * t.lss, e->size * t.lss);
        else
            r = blkpg((int)fd, BLKPG_DEL_, num, 0, 0);
        if (r < 0) {
            snprintf(why, whyn, "the kernel would not update %s (error %ld)",
                     parts[i], -r);
            ok = false;
        }
    }
    /* Add what the kernel does not have. */
    for (int i = 0; i < t.n; i++) {
        char pn[40];
        part_name(disk, t.p[i].num, pn, sizeof pn);
        if (blk_exists(pn))
            continue;
        long r = blkpg((int)fd, BLKPG_ADD_, t.p[i].num, t.p[i].start * t.lss,
                       t.p[i].size * t.lss);
        if (r < 0 && r != -EBUSY_) {
            snprintf(why, whyn, "the kernel would not add %s (error %ld)", pn, -r);
            ok = false;
        }
    }
    lp_close((int)fd);

    /* Check: every table entry is now a kernel partition of the right
     * size, and every one of them has its /dev node. */
    for (int i = 0; ok && i < t.n; i++) {
        char pn[40], p[96], d[64];
        part_name(disk, t.p[i].num, pn, sizeof pn);
        snprintf(p, sizeof p, "/sys/class/block/%s/size", pn);
        if (!blk_exists(pn) || sys_u64(p) != t.p[i].size * f || !dev_path(pn, d, sizeof d)) {
            snprintf(why, whyn, "the kernel's view of %s does not match the table", pn);
            ok = false;
        }
    }
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Running things
 * ═══════════════════════════════════════════════════════════════════ */

/* Absolute paths only, looked up in a fixed order; PATH in the child is
 * set here too. The Debian tools are in /usr/sbin and /usr/bin; /sbin
 * and /bin are for the recovery system, which copies the few it needs
 * next to our own userland. */
static bool tool(const char *name, char *out, size_t n)
{
    static const char *const dirs[] = { "/usr/sbin", "/usr/bin", "/sbin",
                                        "/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, n, "%s/%s", dirs[i], name);
        lp_stat_t st;
        if (lp_stat(out, &st, true) == 0 &&
            (st.mode & LP_S_IFMT) == LP_S_IFREG && (st.mode & 0111))
            return true;
    }
    out[0] = '\0';
    return false;
}

static char *const child_env[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME=/root",
    "LANG=C.UTF-8",
    "LC_ALL=C.UTF-8",
    "TERM=dumb",
    NULL
};

/* What to make of the status pipe (fd 3 in the child), when there is one. */
typedef enum { STAT_NONE, STAT_E2FSCK } statkind_t;

static int pct_lo = 0, pct_hi = 100;    /* the slice of the step a tool gets */
static const char *pct_text = "";

static void scaled(int pct, const char *text)
{
    progress(pct_lo + (pct_hi - pct_lo) * pct / 100, text);
}

static void status_e2fsck(char *line)
{
    int pass = atoi(line);
    char *a = strchr(line, ' ');
    if (!a) return;
    long cur = strtol(a + 1, &a, 10);
    long max = strtol(a, NULL, 10);
    if (pass < 1 || pass > 5 || max <= 0) return;
    char msg[64];
    snprintf(msg, sizeof msg, "%s: pass %d of 5", pct_text, pass);
    scaled((int)(((pass - 1) * 100 + cur * 100 / max) / 5), msg);
}

typedef struct {
    char buf[2048];
    size_t len;
} linebuf_t;

static void feed(linebuf_t *lb, const char *data, long n, void (*fn)(char *))
{
    for (long i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r' || c == '\b') {
            if (lb->len > 0) {
                lb->buf[lb->len] = '\0';
                fn(lb->buf);
                lb->len = 0;
            }
        } else if (lb->len < sizeof lb->buf - 1) {
            lb->buf[lb->len++] = c;
        }
    }
}

/* Tools that print their own progress: resize2fs -p draws a bar of X,
 * ntfsresize prints "NN.NN percent completed". Both become progress. */
static int xs_seen;
static void out_line(char *s)
{
    while (*s == ' ') s++;
    if (!*s)
        return;
    char *pc = strstr(s, " percent completed");
    if (pc) {
        char *b = pc;
        while (b > s && ((b[-1] >= '0' && b[-1] <= '9') || b[-1] == '.')) b--;
        scaled(atoi(b), pct_text);
        return;
    }
    size_t xs = 0;
    for (const char *q = s; *q; q++) if (*q == 'X') xs++;
    if (xs > 10 && xs == strlen(s)) {     /* a resize2fs progress bar row */
        xs_seen += (int)xs;
        scaled(xs_seen % 100, pct_text);
        return;
    }
    say(s);
}

static volatile int cancel_req;          /* SIGUSR1 in the worker */
static void on_cancel(int sig) { (void)sig; cancel_req = 1; }

/* Run one program to the end. argv[0] is an absolute path. stdout and
 * stderr come back to the client as log lines; `input` (may be NULL)
 * is written to its stdin and the pipe closed - how a passphrase or an
 * sfdisk script goes in without ever being an argument, which every
 * user could read in /proc. `cancellable`: a cancel request kills it
 * (only for things that change nothing, like a read-only check).
 * Returns the exit status, -1 when it could not start or was killed. */
static int run_in(char *const argv[], statkind_t sk, const char *input,
                  size_t inlen, bool cancellable, bool secret)
{
    char cmd[900];
    size_t k = strlcpy(cmd, "exec", sizeof cmd);
    for (int i = 0; argv[i] && k < sizeof cmd - 2; i++) {
        k = strlcat(cmd, " ", sizeof cmd);
        k = strlcat(cmd, argv[i], sizeof cmd);
    }
    audit(cmd);
    if (input && !secret) {
        char in[300];
        snprintf(in, sizeof in, "  stdin: %.*s", (int)(inlen > 250 ? 250 : inlen), input);
        for (char *q = in; *q; q++) if (*q == '\n') *q = ' ';
        audit(in);
    }

    int out[2], st[2] = { -1, -1 }, inp[2] = { -1, -1 };
    if (lp_pipe(out) < 0)
        return -1;
    if (sk != STAT_NONE && lp_pipe(st) < 0)
        return -1;
    if (input && lp_pipe(inp) < 0)
        return -1;

    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (input) {
            lp_dup2(inp[0], 0);
        } else {
            long nul = lp_open("/dev/null", O_RDONLY, 0);
            if (nul >= 0) lp_dup2((int)nul, 0);
        }
        lp_dup2(out[1], 1);
        lp_dup2(out[1], 2);
        if (st[1] >= 0)
            lp_dup2(st[1], 3);
        for (int fd = (st[1] >= 0 ? 4 : 3); fd < 1024; fd++)
            lp_close(fd);
        lp_signal_default(13);
        lp_signal_default(SIGINT);
        lp_signal_default(SIGTERM);
        lp_signal_default(SIGUSR1_);
        sys_call1(SYS_umask_, 022);
        lp_chdir("/");
        lp_setsid();
        lp_execve(argv[0], argv, child_env);
        dprintf(2, "cannot run %s\n", argv[0]);
        lp_exit(127);
    }
    lp_close(out[1]);
    if (st[1] >= 0) lp_close(st[1]);
    if (input) {
        lp_close(inp[0]);
        size_t put = 0;
        while (put < inlen) {
            long w = lp_write(inp[1], input + put, inlen - put);
            if (w == -EINTR_) continue;
            if (w <= 0) break;
            put += (size_t)w;
        }
        lp_close(inp[1]);
    }

    static linebuf_t ob, sb;
    ob.len = sb.len = 0;
    xs_seen = 0;
    bool out_open = true, st_open = st[0] >= 0, killed = false;
    while (out_open || st_open) {
        lp_pollfd_t p[2];
        unsigned np = 0;
        int oi = -1, si = -1;
        if (out_open) { p[np].fd = out[0]; p[np].events = LP_POLLIN; p[np].revents = 0; oi = (int)np++; }
        if (st_open)  { p[np].fd = st[0];  p[np].events = LP_POLLIN; p[np].revents = 0; si = (int)np++; }
        long r = lp_poll(p, np, 250);
        if (cancel_req && cancellable && !killed) {
            lp_kill(-pid, SIGTERM);
            killed = true;
        }
        if (r <= 0)
            continue;
        char buf[4096];
        if (oi >= 0 && p[oi].revents) {
            long n = lp_read(out[0], buf, sizeof buf);
            if (n == -EINTR_) continue;
            if (n <= 0) out_open = false;
            else feed(&ob, buf, n, out_line);
        }
        if (si >= 0 && p[si].revents) {
            long n = lp_read(st[0], buf, sizeof buf);
            if (n == -EINTR_) continue;
            if (n <= 0) st_open = false;
            else feed(&sb, buf, n, status_e2fsck);
        }
    }
    if (ob.len) { ob.buf[ob.len] = '\0'; out_line(ob.buf); }
    lp_close(out[0]);
    if (st[0] >= 0) lp_close(st[0]);

    int status = 0;
    while (lp_waitpid(pid, &status, 0) == -EINTR_)
        ;
    int code = (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1;
    char line[64];
    snprintf(line, sizeof line, "  exit %d", code);
    audit(line);
    return code;
}

static int run(char *const argv[])
{
    return run_in(argv, STAT_NONE, NULL, 0, false, false);
}

/* A failure message, the job's result code, and the audit line. */
static int  job_rc;
static char fail_why[16];
static char fail_text[600];

static void done(const char *text)
{
    reply("done", text);
    job_rc = 0;
}

static bool failed(const char *why, const char *text)
{
    strlcpy(fail_why, why, sizeof fail_why);
    strlcpy(fail_text, text, sizeof fail_text);
    job_rc = 1;
    char line[700];
    snprintf(line, sizeof line, "  fail %s %s", why, text);
    audit(line);
    return false;
}

static void finish_fail(void)
{
    char line[700];
    snprintf(line, sizeof line, "%s %s", fail_why[0] ? fail_why : "failed",
             fail_text);
    reply("fail", line);
}

static bool need_tool(const char *name, const char *pkg, char *out, size_t n)
{
    if (tool(name, out, n))
        return true;
    char msg[200];
    snprintf(msg, sizeof msg, "%s is not installed (Debian package %s)", name, pkg);
    return failed("missing", msg);
}
