/* ls - list a directory.
 *
 *   ls [-l] [-a] [-h] [-t] [-S] [-r] [-R] [-d] [-1] [path]...
 *
 *   -l  one per line, with permissions, owner, size and date
 *   -a  include the names beginning with a dot
 *   -h  sizes as 4K and 1M rather than as digits
 *   -t  newest first
 *   -S  largest first
 *   -r  the other way round
 *   -R  and everything underneath
 *   -d  the directory itself, not what is in it
 *   -1  one name per line without the rest of -l
 *
 * getdents64 fills the buffer with variable-length records, back to back:
 *   struct linux_dirent64 {
 *       u64  d_ino;      offset 0
 *       s64  d_off;      offset 8
 *       u16  d_reclen;   offset 16   <- bytes to the next record
 *       u8   d_type;     offset 18
 *       char d_name[];   offset 19   NUL terminated
 *   };
 * We read by offset rather than declaring a struct, so nothing depends on
 * the compiler's padding rules.
 *
 * The names are collected before anything is printed, because sorting
 * needs all of them and because two passes are what lets the columns
 * line up. That puts a ceiling on how many entries one directory can
 * have - a real ls streams - and the ceiling is stated rather than
 * silently truncating.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF_SIZE       8192
#define DIRENT_RECLEN  16
#define DIRENT_TYPE    18
#define DIRENT_NAME    19

#define MAX_ENTRIES  1024
#define NAME_MAX      256

/* d_type values */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

static bool opt_long, opt_all, opt_human, opt_time, opt_size;
static bool opt_reverse, opt_recurse, opt_dironly, opt_one;
static int  failures;

typedef struct {
    char name[NAME_MAX];
    u8   type;
    lp_stat_t st;
    bool have_stat;
} entry_t;

static entry_t entries[MAX_ENTRIES];

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

static u8 type_from_mode(u32 mode)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  return DT_DIR;
    case LP_S_IFLNK:  return DT_LNK;
    case LP_S_IFCHR:  return DT_CHR;
    case LP_S_IFBLK:  return DT_BLK;
    case LP_S_IFIFO:  return DT_FIFO;
    case LP_S_IFSOCK: return DT_SOCK;
    default:          return DT_REG;
    }
}

/* "drwxr-xr-x" */
static void mode_string(u32 mode, char *out)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  out[0] = 'd'; break;
    case LP_S_IFLNK:  out[0] = 'l'; break;
    case LP_S_IFCHR:  out[0] = 'c'; break;
    case LP_S_IFBLK:  out[0] = 'b'; break;
    case LP_S_IFIFO:  out[0] = 'p'; break;
    case LP_S_IFSOCK: out[0] = 's'; break;
    default:          out[0] = '-'; break;
    }
    static const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';

    /* setuid, setgid and the sticky bit sit on top of the x they
     * modify, which is where everyone expects to read them. */
    if (mode & 04000) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (mode & 02000) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (mode & 01000) out[9] = (out[9] == 'x') ? 't' : 'T';
    out[10] = '\0';
}

static void human_size(u64 n, char *out, size_t size)
{
    if (!opt_human) {
        snprintf(out, size, "%lu", (unsigned long)n);
        return;
    }
    static const char *unit[] = { "", "K", "M", "G", "T" };
    int u = 0;
    while (n >= 10240 && u < 4) { n /= 1024; u++; }
    snprintf(out, size, "%lu%s", (unsigned long)n, unit[u]);
}

/* "2026-09-03 08:15". A date nobody has to decode beats the one ls
 * usually prints, which hides the year on anything recent. */
static void time_string(s64 t, char *out, size_t size)
{
    if (t <= 0) { strlcpy(out, "               -", size); return; }
    lp_tm_t tm;
    lp_gmtime(t, &tm);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d",
             tm.year, tm.mon, tm.day, tm.hour, tm.min);
}

static int compare(const entry_t *a, const entry_t *b)
{
    int r;
    if (opt_time)      r = (a->st.mtime < b->st.mtime) ? 1 :
                           (a->st.mtime > b->st.mtime) ? -1 : 0;
    else if (opt_size) r = (a->st.size < b->st.size) ? 1 :
                           (a->st.size > b->st.size) ? -1 : 0;
    else               r = strcmp(a->name, b->name);

    if (r == 0 && (opt_time || opt_size))
        r = strcmp(a->name, b->name);      /* a stable tie-break */
    return opt_reverse ? -r : r;
}

static void sort_entries(int n)
{
    /* Insertion sort. A directory big enough for this to matter is one
     * where reading it costs far more than ordering it. */
    for (int i = 1; i < n; i++) {
        entry_t key = entries[i];
        int j = i - 1;
        while (j >= 0 && compare(&entries[j], &key) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void print_entry(const char *dir, const entry_t *e)
{
    if (!opt_long) {
        char suffix = type_suffix(e->type);
        if (suffix && !opt_one) printf("%s%c\n", e->name, suffix);
        else                    printf("%s\n", e->name);
        return;
    }

    char modes[12] = "----------";
    char size[16]  = "?";
    char when[24]  = "               -";
    char owner[32] = "?", group[32] = "?";

    if (e->have_stat) {
        mode_string(e->st.mode, modes);
        human_size(e->st.size, size, sizeof size);
        time_string(e->st.mtime, when, sizeof when);

        lp_user_t u;
        if (lp_user_by_uid(e->st.uid, &u))
            strlcpy(owner, u.name, sizeof owner);
        else
            snprintf(owner, sizeof owner, "%d", (int)e->st.uid);
        lp_group_name(e->st.gid, group, sizeof group);
    }

    char link[256] = "";
    if (e->type == DT_LNK) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", dir, e->name);
        long n = lp_readlink(full, link, sizeof link - 1);
        if (n > 0) link[n] = '\0';
        else       link[0] = '\0';
    }

    printf("%s %3u %-8s %-8s %8s  %s  %s%s%s\n",
           modes, (unsigned)(e->have_stat ? e->st.nlink : 1),
           owner, group, size, when, e->name,
           link[0] ? " -> " : "", link);
}

static int list_dir(const char *path, bool show_header);

static int list_one(const char *path)
{
    /* -d, or a path that is not a directory: the name is the answer. */
    lp_stat_t st;
    bool have = (lp_stat(path, &st, false) == 0);

    if (opt_dironly || (have && (st.mode & LP_S_IFMT) != LP_S_IFDIR)) {
        if (!have) {
            dprintf(STDERR_FILENO, "ls: %s: no such file\n", path);
            return 1;
        }
        entry_t e;
        memset(&e, 0, sizeof e);
        strlcpy(e.name, path, sizeof e.name);
        e.st = st;
        e.have_stat = true;
        e.type = type_from_mode(st.mode);

        /* The directory the name lives in, for resolving a symlink. */
        char dir[512];
        strlcpy(dir, path, sizeof dir);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else strlcpy(dir, ".", sizeof dir);

        print_entry(dir, &e);
        return 0;
    }

    return list_dir(path, false);
}

static int list_dir(const char *path, bool show_header)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        if (fd == -20 /* ENOTDIR */ || lp_exists(path)) {
            printf("%s\n", path);
            return 0;
        }
        dprintf(STDERR_FILENO, "ls: %s: cannot open (%ld)\n", path, -fd);
        return 1;
    }

    if (show_header)
        printf("\n%s:\n", path);

    int n = 0;
    bool full = false;
    char buf[BUF_SIZE];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got == 0)
            break;
        if (got < 0) {
            dprintf(STDERR_FILENO, "ls: %s: read failed (%ld)\n", path, -got);
            lp_close((int)fd);
            return 1;
        }

        for (long off = 0; off < got; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;

            if (!opt_all && name[0] == '.')
                continue;
            if (n >= MAX_ENTRIES) { full = true; continue; }

            entry_t *e = &entries[n];
            memset(e, 0, sizeof *e);
            strlcpy(e->name, name, sizeof e->name);
            e->type = type;

            /* Only stat when something needs it. On a directory of a
             * few thousand names that is the difference between instant
             * and noticeably slow. */
            if (opt_long || opt_time || opt_size) {
                char full_path[768];
                snprintf(full_path, sizeof full_path, "%s/%s", path, name);
                e->have_stat = (lp_stat(full_path, &e->st, false) == 0);
                if (e->have_stat && type == DT_UNKNOWN)
                    e->type = type_from_mode(e->st.mode);
            }
            n++;
        }
    }
    lp_close((int)fd);

    if (full)
        dprintf(STDERR_FILENO,
                "ls: %s has more than %d entries - only the first %d are\n"
                "ls:   shown. 'find %s' walks it without holding it all.\n",
                path, MAX_ENTRIES, MAX_ENTRIES, path);

    sort_entries(n);

    if (opt_long) {
        u64 blocks = 0;
        for (int i = 0; i < n; i++)
            if (entries[i].have_stat)
                blocks += (entries[i].st.size + 1023) / 1024;
        char t[16];
        human_size(blocks * 1024, t, sizeof t);
        printf("total %s\n", t);
    }

    for (int i = 0; i < n; i++)
        print_entry(path, &entries[i]);

    if (!opt_recurse)
        return 0;

    /* The names are copied out before recursing: the shared table is
     * about to be overwritten by the directory below. */
    static char subdirs[64][NAME_MAX];
    int nsub = 0;
    for (int i = 0; i < n && nsub < 64; i++) {
        if (entries[i].type != DT_DIR)
            continue;
        if (strcmp(entries[i].name, ".") == 0 ||
            strcmp(entries[i].name, "..") == 0)
            continue;
        strlcpy(subdirs[nsub++], entries[i].name, NAME_MAX);
    }

    for (int i = 0; i < nsub; i++) {
        char child[768];
        snprintf(child, sizeof child, "%s/%s", path, subdirs[i]);
        failures |= list_dir(child, true);
    }
    return 0;
}

static void usage(void)
{
    printf("usage: ls [-lahtSrRd1] [path]...\n\n");
    printf("  -l  permissions, owner, size and date\n");
    printf("  -a  the names beginning with a dot too\n");
    printf("  -h  sizes as 4K and 1M\n");
    printf("  -t  newest first\n");
    printf("  -S  largest first\n");
    printf("  -r  the other way round\n");
    printf("  -R  and everything underneath\n");
    printf("  -d  the directory itself, not what is in it\n");
    printf("  -1  one name per line\n");
}

int main(int argc, char **argv)
{
    const char *paths[64];
    int npaths = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *o = argv[i] + 1; *o; o++) {
                switch (*o) {
                case 'l': opt_long = true; break;
                case 'a': opt_all = true; break;
                case 'h': opt_human = true; break;
                case 't': opt_time = true; break;
                case 'S': opt_size = true; break;
                case 'r': opt_reverse = true; break;
                case 'R': opt_recurse = true; break;
                case 'd': opt_dironly = true; break;
                case '1': opt_one = true; break;
                default:
                    dprintf(STDERR_FILENO, "ls: unknown option -%c\n", *o);
                    usage();
                    return 2;
                }
            }
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        if (npaths < 64)
            paths[npaths++] = argv[i];
    }

    if (npaths == 0)
        return list_one(".");

    for (int i = 0; i < npaths; i++) {
        if (npaths > 1 && !opt_dironly && lp_is_dir(paths[i]))
            printf("%s%s:\n", i ? "\n" : "", paths[i]);
        failures |= list_one(paths[i]);
    }
    return failures;
}
