/* tar - make and open archives.
 *
 *   tar -c <archive> <path>...     create
 *   tar -t <archive>               list what is inside
 *   tar -x <archive> [dir]         extract (into dir, default here)
 *
 * Uncompressed ustar, which is what pkg uses: a package is a tar and
 * this is how one gets made or looked inside on the machine itself.
 * Without it, packages could only be built somewhere else and only
 * inspected by installing them.
 *
 * No compression. The only decompressor on this system is inside the
 * kernel and not reachable from here; the archive is larger and the
 * code that has to be right is a third of the size, which for something
 * that runs as root and unpacks files someone else made is the better
 * trade.
 *
 * Extraction refuses absolute paths and any component that is "..".
 * That is how an archive escapes the directory it was meant to stay in,
 * and it has been used in anger for thirty years.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BLOCK        512
#define NAME_OFF       0
#define MODE_OFF     100
#define SIZE_OFF     124
#define MTIME_OFF    136
#define CHKSUM_OFF   148
#define TYPE_OFF     156
#define LINK_OFF     157
#define MAGIC_OFF    257
#define PREFIX_OFF   345

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

static u8 block[BLOCK];

static u64 from_octal(const u8 *field, int len)
{
    u64 v = 0;
    for (int i = 0; i < len; i++) {
        if (field[i] < '0' || field[i] > '7')
            continue;
        v = v * 8 + (u64)(field[i] - '0');
    }
    return v;
}

static void to_octal(u8 *field, int len, u64 v)
{
    /* Right-aligned, zero-padded, with a NUL in the last position -
     * which is what every tar since 1979 expects to read. */
    for (int i = len - 2; i >= 0; i--) {
        field[i] = (u8)('0' + (v & 7));
        v >>= 3;
    }
    field[len - 1] = '\0';
}

/* The header checksum is the sum of every byte with the checksum field
 * itself treated as spaces. */
static void set_checksum(u8 *hdr)
{
    memset(hdr + CHKSUM_OFF, ' ', 8);

    u32 sum = 0;
    for (int i = 0; i < BLOCK; i++)
        sum += hdr[i];

    to_octal(hdr + CHKSUM_OFF, 7, sum);
    hdr[CHKSUM_OFF + 7] = ' ';
}

static bool path_is_safe(const char *p)
{
    if (p[0] == '/')
        return false;
    for (const char *q = p; *q; ) {
        if (q[0] == '.' && q[1] == '.' && (q[2] == '/' || q[2] == '\0'))
            return false;
        while (*q && *q != '/') q++;
        while (*q == '/') q++;
    }
    return true;
}

static void make_parents(const char *path)
{
    char work[1024];
    strlcpy(work, path, sizeof(work));
    for (char *p = work + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!lp_is_dir(work))
            lp_mkdir(work, 0755);
        *p = '/';
    }
}

/* ── Creating ─────────────────────────────────────────────────────── */

static bool write_header(int out, const char *name, u64 size,
                         u32 mode, char type)
{
    memset(block, 0, BLOCK);

    if (strlen(name) > 99) {
        dprintf(STDERR_FILENO, "tar: name too long: %s\n", name);
        return false;
    }
    strlcpy((char *)block + NAME_OFF, name, 100);

    to_octal(block + MODE_OFF, 8, mode & 07777);
    to_octal(block + MODE_OFF + 8, 8, 0);          /* uid */
    to_octal(block + MODE_OFF + 16, 8, 0);         /* gid */
    to_octal(block + SIZE_OFF, 12, size);
    to_octal(block + MTIME_OFF, 12, (u64)lp_time());
    block[TYPE_OFF] = (u8)type;
    memcpy(block + MAGIC_OFF, "ustar\00000", 8);

    set_checksum(block);
    return lp_write(out, block, BLOCK) == BLOCK;
}

static bool add_path(int out, const char *path);

static bool add_dir(int out, const char *path)
{
    if (!write_header(out, path, 0, 0755, '5'))
        return false;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return true;

    char buf[8192];
    bool ok = true;

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, name);
            if (!add_path(out, child))
                ok = false;
        }
    }
    lp_close((int)fd);
    return ok;
}

static bool add_file(int out, const char *path, const lp_stat_t *st)
{
    long in = lp_open(path, O_RDONLY, 0);
    if (in < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot read\n", path);
        return false;
    }

    if (!write_header(out, path, st->size, st->mode, '0')) {
        lp_close((int)in);
        return false;
    }

    u64 left = st->size;
    while (left > 0) {
        memset(block, 0, BLOCK);
        size_t want = left < BLOCK ? (size_t)left : BLOCK;
        long   got  = lp_read((int)in, block, want);
        if (got <= 0)
            break;
        lp_write(out, block, BLOCK);        /* always a whole block */
        left -= (u64)got;
    }
    lp_close((int)in);

    /* The file shrank while we were reading it: pad out so the archive
     * still lines up with the size in the header. */
    while (left > 0) {
        memset(block, 0, BLOCK);
        lp_write(out, block, BLOCK);
        left = left > BLOCK ? left - BLOCK : 0;
    }
    return true;
}

static bool add_path(int out, const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, false) < 0) {
        dprintf(STDERR_FILENO, "tar: %s: not there\n", path);
        return false;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
        char target[128];
        long n = lp_readlink(path, target, sizeof(target) - 1);
        if (n <= 0)
            return false;
        target[n] = '\0';

        memset(block, 0, BLOCK);
        strlcpy((char *)block + NAME_OFF, path, 100);
        strlcpy((char *)block + LINK_OFF, target, 100);
        to_octal(block + MODE_OFF, 8, 0777);
        to_octal(block + MODE_OFF + 8, 8, 0);
        to_octal(block + MODE_OFF + 16, 8, 0);
        to_octal(block + SIZE_OFF, 12, 0);
        to_octal(block + MTIME_OFF, 12, (u64)lp_time());
        block[TYPE_OFF] = '2';
        memcpy(block + MAGIC_OFF, "ustar\00000", 8);
        set_checksum(block);
        return lp_write(out, block, BLOCK) == BLOCK;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR)
        return add_dir(out, path);

    return add_file(out, path, &st);
}

static int create(const char *archive, char **paths, int npaths)
{
    long out = lp_open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot create\n", archive);
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < npaths; i++)
        if (!add_path((int)out, paths[i]))
            rc = 1;

    /* Two empty blocks mark the end. */
    memset(block, 0, BLOCK);
    lp_write((int)out, block, BLOCK);
    lp_write((int)out, block, BLOCK);

    lp_close((int)out);
    return rc;
}

/* ── Listing and extracting ───────────────────────────────────────── */

static int walk(const char *archive, const char *into, bool extract)
{
    long fd = lp_open(archive, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot open\n", archive);
        return 1;
    }

    int empty = 0;
    int rc = 0;

    for (;;) {
        long n = lp_read((int)fd, block, BLOCK);
        if (n <= 0)
            break;
        if (n < BLOCK) {
            dprintf(STDERR_FILENO, "tar: %s: truncated\n", archive);
            rc = 1;
            break;
        }

        bool zero = true;
        for (int i = 0; i < BLOCK; i++)
            if (block[i]) { zero = false; break; }
        if (zero) {
            if (++empty >= 2) break;
            continue;
        }
        empty = 0;

        char base[101], prefix[156], name[300];
        memcpy(base, block + NAME_OFF, 100);   base[100] = '\0';
        memcpy(prefix, block + PREFIX_OFF, 155); prefix[155] = '\0';
        if (prefix[0]) snprintf(name, sizeof(name), "%s/%s", prefix, base);
        else           strlcpy(name, base, sizeof(name));

        u64  size   = from_octal(block + SIZE_OFF, 12);
        u32  mode   = (u32)from_octal(block + MODE_OFF, 8);
        char type   = (char)block[TYPE_OFF];
        u64  blocks = (size + BLOCK - 1) / BLOCK;

        if (!extract) {
            const char *what = type == '5' ? "dir " : type == '2' ? "link" : "file";
            printf("%s %8llu  %s\n", what, (unsigned long long)size, name);
            for (u64 b = 0; b < blocks; b++)
                lp_read((int)fd, block, BLOCK);
            continue;
        }

        if (!path_is_safe(name)) {
            dprintf(STDERR_FILENO,
                    "tar: refusing '%s' - it points outside %s\n", name, into);
            lp_close((int)fd);
            return 1;
        }

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", into, name);

        if (type == '5') {
            make_parents(full);
            if (!lp_is_dir(full))
                lp_mkdir(full, mode ? mode : 0755);
            continue;
        }

        if (type == '2') {
            char target[101];
            memcpy(target, block + LINK_OFF, 100);
            target[100] = '\0';
            make_parents(full);
            lp_unlink(full);
            lp_symlink(target, full);
            continue;
        }

        make_parents(full);
        long out = lp_open(full, O_WRONLY | O_CREAT | O_TRUNC,
                           mode ? (mode & 0777) : 0644);
        if (out < 0) {
            dprintf(STDERR_FILENO, "tar: cannot write %s\n", full);
            rc = 1;
            for (u64 b = 0; b < blocks; b++)
                lp_read((int)fd, block, BLOCK);
            continue;
        }

        u64 left = size;
        for (u64 b = 0; b < blocks; b++) {
            if (lp_read((int)fd, block, BLOCK) < BLOCK)
                break;
            size_t want = left < BLOCK ? (size_t)left : BLOCK;
            lp_write((int)out, block, want);
            left -= want;
        }
        lp_close((int)out);
        if (mode)
            lp_chmod(full, mode & 0777);
    }

    lp_close((int)fd);
    return rc;
}

/* GNU tar 로 넘길 것인가.
 *
 * 이 tar 는 pkg 가 쓰는 ustar 부분집합만 안다. 그것으로 충분한
 * 이유는 이 OS 의 패키지가 그 형식이기 때문이고, 충분하지 않은
 * 이유는 이 기계에 데비안도 같이 있기 때문이다: dpkg-deb 는
 * `tar --warning=no-timestamp -xf -` 처럼 부르고, PATH 가 /bin 을
 * 먼저 보므로 그 tar 가 우리 것이 된다. 우리 것은 그 인자를 모르고,
 * dpkg 는 "tar subprocess returned error exit status 1" 만 남긴다 -
 * 무엇이 없는지 아무도 알 수 없는 실패다.
 *
 * 그래서 우리가 아는 꼴이 아니면 GNU tar 에게 넘긴다. 우리 것이
 * 앞에 있다는 사실은 그대로 두면서(사람이 `tar -t pkg.tar` 를 치면
 * 우리 것이 답한다) 기계가 우분투처럼 동작하게 하는 방법이다.
 * GNU tar 가 없는 기계에서는 예전처럼 우리가 답한다. */
static void hand_to_gnu_tar(int argc, char **argv)
{
    static const char *GNU = "/usr/bin/tar";
    if (lp_access(GNU, 1) != 0)
        return;                        /* 데비안이 깔리지 않은 이미지 */

    (void)argc;
    lp_execve(GNU, argv, environ);
    /* 넘기지 못하면 그냥 우리가 계속한다. */
}

/* 우리가 다룰 수 있는 꼴인가: -c/-t/-x 하나에 파일 이름. */
static bool ours(int argc, char **argv)
{
    if (argc < 3)
        return false;
    if (strcmp(argv[1], "-c") != 0 && strcmp(argv[1], "-t") != 0 &&
        strcmp(argv[1], "-x") != 0)
        return false;
    /* 긴 옵션이 하나라도 있으면 GNU 것이다. '-' (표준 입력) 도. */
    for (int i = 2; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1])
            return false;
    if (strcmp(argv[2], "-") == 0)
        return false;
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !ours(argc, argv) && strcmp(argv[1], "-h") != 0)
        hand_to_gnu_tar(argc, argv);

    if (argc < 3 || strcmp(argv[1], "-h") == 0) {
        printf("usage:\n");
        printf("  tar -c <archive> <path>...   create\n");
        printf("  tar -t <archive>             list\n");
        printf("  tar -x <archive> [dir]       extract\n\n");
        printf("Uncompressed ustar - the format pkg uses, so this is how\n");
        printf("a package is made or looked inside on the machine.\n");
        return argc < 3 ? 2 : 0;
    }

    const char *mode    = argv[1];
    const char *archive = argv[2];

    if (strcmp(mode, "-c") == 0) {
        if (argc < 4) {
            dprintf(STDERR_FILENO, "tar: nothing to put in it\n");
            return 2;
        }
        return create(archive, argv + 3, argc - 3);
    }

    if (strcmp(mode, "-t") == 0)
        return walk(archive, ".", false);

    if (strcmp(mode, "-x") == 0)
        return walk(archive, argc > 3 ? argv[3] : ".", true);

    dprintf(STDERR_FILENO, "tar: -c, -t or -x\n");
    return 2;
}
