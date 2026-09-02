/* pkg - install and remove packages.
 *
 *   pkg list                 what is installed
 *   pkg info <name>          what one package is, and what it put where
 *   pkg add <file.tar>       install from a file you already have
 *   pkg remove <name>        take it out again
 *   pkg repo [url]           show or set where packages come from
 *   pkg update               fetch the repository's index
 *   pkg search <text>        what the repository has
 *   pkg install <name>...    fetch, check and install
 *
 * ── The format ──
 * A package is a plain uncompressed tar. Not a format of our own: tar is
 * made by every machine on earth, so a package can be built with one
 * command and inspected with another, and there is nothing to learn.
 * Uncompressed because the only decompressor on this system is inside
 * the kernel and not reachable from here - the download is larger, the
 * code that has to be right is a third of the size, and that is the
 * better trade for something that runs as root.
 *
 * Paths in the archive are relative and land under /data. A package
 * holding bin/foo installs /data/bin/foo, which is on PATH already. An
 * absolute path, or one containing "..", is refused outright: an archive
 * is untrusted input, and that is exactly how an archive escapes the
 * directory it was meant to stay in.
 *
 * ── What the checking does and does not cover ──
 * Everything downloaded is checked against the SHA-256 in the index, so
 * a corrupted transfer or a mirror serving the wrong bytes is caught.
 * The index itself arrives over HTTP, and this machine has no TLS in C -
 * so someone able to rewrite your traffic could serve their own index
 * and their own packages, and the hashes would agree with each other.
 *
 * That is a real limit and worth stating plainly rather than implying a
 * safety that is not there. When it matters: fetch the file yourself
 * over HTTPS with python3, check it, and use "pkg add". That path never
 * touches the network here at all.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define ROOT      "/data"
#define DB_DIR    "/data/pkg/db"
#define REPO_FILE "/data/pkg/repo"
#define INDEX     "/data/pkg/index"
#define TMP_FILE  "/data/pkg/.download"

/* ═══════════════════════════════════════════════════════════════════
 * SHA-256
 *
 * Written out here rather than pulled in from somewhere: it is a hundred
 * lines, it has no dependencies, and it is the one thing standing
 * between "the bytes arrived" and "the bytes are the ones we asked for".
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    u32 h[8];
    u64 len;
    u8  buf[64];
    int used;
} sha256_t;

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static u32 ror(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(sha256_t *s, const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) |
               ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; i++) {
        u32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->used = 0;
}

static void sha256_update(sha256_t *s, const u8 *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - (size_t)s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += (int)take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha256_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha256_final(sha256_t *s, char *hex)
{
    u64 bits = s->len * 8;

    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    u8 zero = 0;
    while (s->used != 56)
        sha256_update(s, &zero, 1);

    u8 lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (56 - i * 8));
    /* Straight into the block: update would count these bytes again. */
    memcpy(s->buf + 56, lenb, 8);
    sha256_block(s, s->buf);

    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 4; b++) {
            u8 byte = (u8)(s->h[i] >> (24 - b * 8));
            *hex++ = digits[byte >> 4];
            *hex++ = digits[byte & 15];
        }
    *hex = '\0';
}

/* The hash of a file, as 64 hex characters. false if it cannot be read. */
static bool sha256_file(const char *path, char *hex)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;

    sha256_t s;
    sha256_init(&s);

    static u8 buf[8192];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        sha256_update(&s, buf, (size_t)n);
    }
    lp_close((int)fd);

    sha256_final(&s, hex);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Reading a tar
 * ═══════════════════════════════════════════════════════════════════ */
#define TAR_BLOCK      512
#define TAR_NAME         0
#define TAR_MODE       100
#define TAR_SIZE       124
#define TAR_TYPE       156
#define TAR_LINKNAME   157
#define TAR_PREFIX     345

/* Octal fields, which tar pads with spaces or NULs. */
static u64 tar_octal(const u8 *field, int len)
{
    u64 v = 0;
    for (int i = 0; i < len; i++) {
        if (field[i] < '0' || field[i] > '7')
            continue;
        v = v * 8 + (u64)(field[i] - '0');
    }
    return v;
}

/* Is this path safe to write under our root?
 *
 * An archive is something someone else made. Two things in a path let it
 * write outside the directory we intended, and both have been used in
 * anger for thirty years: an absolute path, and "..". */
static bool path_is_safe(const char *p)
{
    if (p[0] == '/')
        return false;

    for (const char *q = p; *q; ) {
        if (q[0] == '.' && q[1] == '.' &&
            (q[2] == '/' || q[2] == '\0'))
            return false;
        while (*q && *q != '/') q++;
        while (*q == '/') q++;
    }
    return true;
}

/* Make the directories leading up to a file. */
static void make_parents(const char *path)
{
    char work[1024];
    strlcpy(work, path, sizeof(work));

    for (char *p = work + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!lp_is_dir(work))
            lp_mkdir(work, 0755);
        *p = '/';
    }
}

/* Unpack one tar into ROOT, writing each path installed to `list_fd`.
 * Returns the number of files, or -1. */
static long tar_extract(const char *archive, int list_fd, bool dry_run)
{
    long fd = lp_open(archive, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "pkg: %s: cannot open\n", archive);
        return -1;
    }

    static u8 hdr[TAR_BLOCK];
    static u8 data[TAR_BLOCK];
    long count = 0;
    int  empty = 0;

    for (;;) {
        long n = lp_read((int)fd, hdr, TAR_BLOCK);
        if (n <= 0)
            break;
        if (n < TAR_BLOCK) {
            dprintf(STDERR_FILENO, "pkg: %s: truncated\n", archive);
            lp_close((int)fd);
            return -1;
        }

        /* Two blocks of zeroes end the archive. */
        bool all_zero = true;
        for (int i = 0; i < TAR_BLOCK; i++)
            if (hdr[i]) { all_zero = false; break; }
        if (all_zero) {
            if (++empty >= 2)
                break;
            continue;
        }
        empty = 0;

        /* The name and prefix fields are fixed width and only NUL
         * terminated when the name is shorter than the field, so they
         * are copied out by length rather than read as strings. */
        char base[101], prefix[156], name[300];
        memcpy(base, hdr + TAR_NAME, 100);
        base[100] = '\0';
        memcpy(prefix, hdr + TAR_PREFIX, 155);
        prefix[155] = '\0';

        if (prefix[0])
            snprintf(name, sizeof(name), "%s/%s", prefix, base);
        else
            strlcpy(name, base, sizeof(name));

        u64  size = tar_octal(hdr + TAR_SIZE, 12);
        u32  mode = (u32)tar_octal(hdr + TAR_MODE, 8);
        char type = (char)hdr[TAR_TYPE];
        u64  blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;

        if (!path_is_safe(name)) {
            dprintf(STDERR_FILENO,
                    "pkg: %s: refusing '%s' - it points outside %s\n",
                    archive, name, ROOT);
            lp_close((int)fd);
            return -1;
        }

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", ROOT, name);

        if (type == '5') {                        /* a directory */
            if (!dry_run) {
                make_parents(full);
                if (!lp_is_dir(full))
                    lp_mkdir(full, mode ? mode : 0755);
            }
            continue;                             /* not recorded: shared */
        }

        if (type == '2') {                        /* a symlink */
            char target[101];
            memcpy(target, hdr + TAR_LINKNAME, 100);
            target[100] = '\0';
            if (!dry_run) {
                make_parents(full);
                lp_unlink(full);
                lp_symlink(target, full);
                if (list_fd >= 0) {
                    dprintf(list_fd, "%s\n", full);
                }
            }
            count++;
            continue;
        }

        if (type != '0' && type != '\0') {
            /* Character devices, fifos, hard links. Skipping them beats
             * half-supporting them. */
            dprintf(STDERR_FILENO, "pkg:   skipping %s (type %c)\n",
                    name, type ? type : '0');
            for (u64 b = 0; b < blocks; b++)
                lp_read((int)fd, data, TAR_BLOCK);
            continue;
        }

        /* A regular file. */
        long out = -1;
        if (!dry_run) {
            make_parents(full);
            out = lp_open(full, O_WRONLY | O_CREAT | O_TRUNC,
                          mode ? (mode & 0777) : 0644);
            if (out < 0) {
                dprintf(STDERR_FILENO, "pkg: cannot write %s\n", full);
                lp_close((int)fd);
                return -1;
            }
        }

        u64 left = size;
        for (u64 b = 0; b < blocks; b++) {
            if (lp_read((int)fd, data, TAR_BLOCK) < TAR_BLOCK) {
                dprintf(STDERR_FILENO, "pkg: %s: truncated inside %s\n",
                        archive, name);
                if (out >= 0) lp_close((int)out);
                lp_close((int)fd);
                return -1;
            }
            size_t want = left < TAR_BLOCK ? (size_t)left : TAR_BLOCK;
            if (out >= 0)
                lp_write((int)out, data, want);
            left -= want;
        }

        if (out >= 0) {
            lp_close((int)out);
            /* The mode again: O_CREAT only applies it to a new file, and
             * an upgrade overwrites an existing one. */
            if (mode)
                lp_chmod(full, mode & 0777);
            if (list_fd >= 0)
                dprintf(list_fd, "%s\n", full);
        }
        count++;
    }

    lp_close((int)fd);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 * HTTP
 *
 * GET, one connection, no redirects, no chunked encoding. A repository
 * is a directory of static files on a web server; anything fancier than
 * that is the server's choice to make and not one we have to follow.
 * ═══════════════════════════════════════════════════════════════════ */

/* Split "http://host[:port]/path" apart. false if it is not one. */
static bool url_split(const char *url, char *host, size_t hsize,
                      int *port, char *path, size_t psize)
{
    static const char scheme[] = "http://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0)
        return false;

    const char *p = url + sizeof(scheme) - 1;
    size_t i = 0;
    *port = 80;

    while (*p && *p != '/' && *p != ':' && i < hsize - 1)
        host[i++] = *p++;
    host[i] = '\0';
    if (i == 0)
        return false;

    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && *p != '/') p++;
    }

    strlcpy(path, *p ? p : "/", psize);
    return true;
}

/* Fetch a URL into a file. Returns the bytes written, or -1. */
static long http_get(const char *url, const char *dest)
{
    char host[128], path[512];
    int  port;

    if (!url_split(url, host, sizeof(host), &port, path, sizeof(path))) {
        dprintf(STDERR_FILENO,
                "pkg: %s: only http:// URLs work here\n"
                "     (there is no TLS in this userland - see 'pkg -h')\n",
                url);
        return -1;
    }

    u32 addr = net_resolve(host);
    if (addr == 0) {
        dprintf(STDERR_FILENO, "pkg: cannot resolve %s\n", host);
        return -1;
    }

    long fd = lp_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    s64 tv[2] = { 20, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u16)port);
    sa.sin_addr   = addr;

    if (lp_connect((int)fd, &sa, sizeof(sa)) < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot connect to %s:%d\n", host, port);
        lp_close((int)fd);
        return -1;
    }

    char req[768];
    int  rn = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.0\r\n"
                       "Host: %s\r\n"
                       "User-Agent: lpzero-pkg\r\n"
                       "Connection: close\r\n\r\n", path, host);
    lp_write((int)fd, req, (size_t)rn);

    long out = lp_open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot write %s\n", dest);
        lp_close((int)fd);
        return -1;
    }

    /* Read past the headers. The blank line between them and the body may
     * land anywhere in a read, so we look for it as we go. */
    static char buf[8192];
    bool  in_body = false;
    int   match   = 0;          /* how much of \r\n\r\n we have seen */
    long  written = 0;
    int   status  = 0;
    bool  have_status = false;

    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        long i = 0;
        if (!in_body) {
            if (!have_status && n > 12) {
                /* "HTTP/1.1 200 OK" */
                status = atoi(buf + 9);
                have_status = true;
            }
            for (; i < n; i++) {
                char c = buf[i];
                if ((match == 0 || match == 2) && c == '\r')      match++;
                else if ((match == 1 || match == 3) && c == '\n') match++;
                else                                              match = (c == '\r');
                if (match == 4) { i++; in_body = true; break; }
            }
        }

        if (in_body && i < n) {
            lp_write((int)out, buf + i, (size_t)(n - i));
            written += n - i;
        }
    }

    lp_close((int)out);
    lp_close((int)fd);

    if (have_status && status != 200) {
        dprintf(STDERR_FILENO, "pkg: %s: the server said %d\n", url, status);
        lp_unlink(dest);
        return -1;
    }
    return written;
}

/* ═══════════════════════════════════════════════════════════════════
 * The package database
 * ═══════════════════════════════════════════════════════════════════ */

static void ensure_dirs(void)
{
    lp_mkdir("/data/pkg", 0755);
    lp_mkdir(DB_DIR, 0755);
}

static void db_path(char *out, size_t size, const char *name, const char *ext)
{
    snprintf(out, size, "%s/%s.%s", DB_DIR, name, ext);
}

static bool is_installed(const char *name)
{
    char p[256];
    db_path(p, sizeof(p), name, "list");
    return lp_exists(p);
}

static int cmd_list(void)
{
    long fd = lp_open(DB_DIR, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        printf("nothing installed\n");
        return 0;
    }

    char buf[8192];
    int  found = 0;

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + 16);
            const char *name = rec + 19;
            off += len;

            size_t l = strlen(name);
            if (l < 6 || strcmp(name + l - 5, ".list") != 0)
                continue;

            char base[128];
            strlcpy(base, name, l - 4 > sizeof(base) ? sizeof(base) : l - 4);

            char info[256], text[256];
            db_path(info, sizeof(info), base, "info");
            if (proc_read(info, text, sizeof(text)) > 0) {
                char *nl = strchr(text, '\n');
                if (nl) *nl = '\0';
                printf("  %-20s %s\n", base, text);
            } else {
                printf("  %s\n", base);
            }
            found++;
        }
    }
    lp_close((int)fd);

    if (!found)
        printf("nothing installed\n");
    return 0;
}

static int cmd_info(const char *name)
{
    if (!is_installed(name)) {
        dprintf(STDERR_FILENO, "pkg: %s is not installed\n", name);
        return 1;
    }

    char p[256], text[256];
    db_path(p, sizeof(p), name, "info");
    if (proc_read(p, text, sizeof(text)) > 0)
        printf("%s", text);

    printf("\nfiles:\n");
    db_path(p, sizeof(p), name, "list");
    long fd = lp_open(p, O_RDONLY, 0);
    if (fd < 0)
        return 1;

    char line[1024];
    long count = 0;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        printf("  %s\n", line);
        count++;
    }
    lp_close((int)fd);
    printf("(%ld)\n", count);
    return 0;
}

static int install_tar(const char *name, const char *version,
                       const char *archive)
{
    ensure_dirs();

    /* Walk it once without writing anything. A package that is going to
     * be refused halfway through should be refused before the first file
     * of it lands on the disk. */
    if (tar_extract(archive, -1, true) < 0)
        return 1;

    char listp[256];
    db_path(listp, sizeof(listp), name, "list");

    long list_fd = lp_open(listp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (list_fd < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot write %s\n", listp);
        return 1;
    }

    long n = tar_extract(archive, (int)list_fd, false);
    lp_close((int)list_fd);

    if (n < 0) {
        lp_unlink(listp);
        return 1;
    }

    char infop[256];
    db_path(infop, sizeof(infop), name, "info");
    long info_fd = lp_open(infop, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (info_fd >= 0) {
        dprintf((int)info_fd, "%s\n", version ? version : "(unknown version)");
        lp_close((int)info_fd);
    }

    lp_sync();
    printf("pkg: installed %s (%ld files)\n", name, n);
    return 0;
}

/* "some/path/foo-1.2.tar" -> "foo" */
static void name_from_file(const char *path, char *out, size_t size)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    strlcpy(out, base, size);

    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".tar") == 0)
        *dot = '\0';

    char *dash = strrchr(out, '-');
    if (dash && dash[1] >= '0' && dash[1] <= '9')
        *dash = '\0';               /* the version, which we keep apart */
}

static int cmd_add(const char *file)
{
    char name[128];
    name_from_file(file, name, sizeof(name));

    const char *base = strrchr(file, '/');
    return install_tar(name, base ? base + 1 : file, file);
}

static int cmd_remove(const char *name)
{
    if (!is_installed(name)) {
        dprintf(STDERR_FILENO, "pkg: %s is not installed\n", name);
        return 1;
    }

    char listp[256];
    db_path(listp, sizeof(listp), name, "list");

    long fd = lp_open(listp, O_RDONLY, 0);
    if (fd < 0)
        return 1;

    char line[1024];
    long gone = 0, kept = 0;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '\0')
            continue;
        if (lp_unlink(line) == 0)
            gone++;
        else
            kept++;
    }
    lp_close((int)fd);

    lp_unlink(listp);
    char infop[256];
    db_path(infop, sizeof(infop), name, "info");
    lp_unlink(infop);

    lp_sync();
    printf("pkg: removed %s (%ld files", name, gone);
    if (kept)
        printf(", %ld were already gone", kept);
    printf(")\n");

    /* Directories are left behind on purpose: several packages share
     * bin/ and lib/, and there is no counting of who else is in them. */
    return 0;
}

static bool repo_url(char *out, size_t size)
{
    char buf[512];
    if (proc_read(REPO_FILE, buf, sizeof(buf)) <= 0)
        return false;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (!buf[0])
        return false;
    strlcpy(out, buf, size);
    return true;
}

static int cmd_repo(const char *url)
{
    char cur[512];

    if (!url) {
        if (repo_url(cur, sizeof(cur)))
            printf("%s\n", cur);
        else
            printf("no repository set. Set one with:\n"
                   "  pkg repo http://your.server/lpzero\n");
        return 0;
    }

    ensure_dirs();
    long fd = lp_open(REPO_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot write %s\n", REPO_FILE);
        return 1;
    }
    dprintf((int)fd, "%s\n", url);
    lp_close((int)fd);
    printf("pkg: repository is %s\n", url);
    printf("     run 'pkg update' to fetch its index\n");
    return 0;
}

static int cmd_update(void)
{
    char base[512];
    if (!repo_url(base, sizeof(base))) {
        dprintf(STDERR_FILENO, "pkg: no repository set (pkg repo <url>)\n");
        return 1;
    }

    ensure_dirs();

    char url[640];
    snprintf(url, sizeof(url), "%s/index", base);

    long n = http_get(url, INDEX);
    if (n < 0)
        return 1;

    printf("pkg: index updated (%ld bytes)\n", n);
    return 0;
}

/* One line of the index: name version size sha256 file */
typedef struct {
    char name[64], version[32], sha[72], file[128];
    long size;
} entry_t;

static bool index_find(const char *name, entry_t *e)
{
    long fd = lp_open(INDEX, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[512];
    bool found = false;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *p = line;
        char *fields[5];
        int   nf = 0;

        while (*p && nf < 5) {
            while (*p == ' ') p++;
            if (!*p) break;
            fields[nf++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 5)
            continue;
        if (strcmp(fields[0], name) != 0)
            continue;

        strlcpy(e->name,    fields[0], sizeof(e->name));
        strlcpy(e->version, fields[1], sizeof(e->version));
        e->size = strtol(fields[2], NULL, 10);
        strlcpy(e->sha,     fields[3], sizeof(e->sha));
        strlcpy(e->file,    fields[4], sizeof(e->file));
        found = true;
        break;
    }

    lp_close((int)fd);
    return found;
}

static int cmd_search(const char *text)
{
    long fd = lp_open(INDEX, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "pkg: no index yet. Set a repository and run 'pkg update'\n");
        return 1;
    }

    char line[512];
    int  hits = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (text && !strstr(line, text))
            continue;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *ver = sp + 1;
        char *sp2 = strchr(ver, ' ');
        if (sp2) *sp2 = '\0';

        printf("  %-20s %-12s %s\n", line, ver,
               is_installed(line) ? "(installed)" : "");
        hits++;
    }
    lp_close((int)fd);

    if (!hits)
        printf("nothing matched\n");
    return 0;
}

static int cmd_install(const char *name)
{
    char base[512];
    if (!repo_url(base, sizeof(base))) {
        dprintf(STDERR_FILENO,
                "pkg: no repository set.\n"
                "     pkg repo <url>, then pkg update\n"
                "     or install a file you already have: pkg add <file.tar>\n");
        return 1;
    }

    entry_t e;
    if (!index_find(name, &e)) {
        dprintf(STDERR_FILENO,
                "pkg: %s is not in the index (try 'pkg update')\n", name);
        return 1;
    }

    ensure_dirs();

    char url[700];
    snprintf(url, sizeof(url), "%s/%s", base, e.file);
    printf("pkg: fetching %s %s\n", e.name, e.version);

    long n = http_get(url, TMP_FILE);
    if (n < 0)
        return 1;

    if (e.size > 0 && n != e.size) {
        dprintf(STDERR_FILENO,
                "pkg: %s: got %ld bytes, the index says %ld\n",
                name, n, e.size);
        lp_unlink(TMP_FILE);
        return 1;
    }

    char hex[72];
    if (!sha256_file(TMP_FILE, hex)) {
        lp_unlink(TMP_FILE);
        return 1;
    }

    if (strcmp(hex, e.sha) != 0) {
        dprintf(STDERR_FILENO,
                "pkg: %s: the checksum does not match. Not installing.\n"
                "     expected %s\n"
                "     got      %s\n", name, e.sha, hex);
        lp_unlink(TMP_FILE);
        return 1;
    }

    int rc = install_tar(e.name, e.version, TMP_FILE);
    lp_unlink(TMP_FILE);
    return rc;
}

static void usage(void)
{
    printf("usage: pkg <command>\n\n");
    printf("  list                 what is installed\n");
    printf("  info <name>          what it is and where its files went\n");
    printf("  add <file.tar>       install from a file you already have\n");
    printf("  remove <name>        take it out again\n");
    printf("  repo [url]           show or set where packages come from\n");
    printf("  update               fetch the repository's index\n");
    printf("  search [text]        what the repository has\n");
    printf("  install <name>...    fetch, check and install\n");
    printf("  sha256 <file>        the checksum of a file\n\n");
    printf("Packages are plain uncompressed tar files. Paths inside them\n");
    printf("are relative and land under %s, so bin/foo becomes\n", ROOT);
    printf("%s/bin/foo - which is already on PATH.\n\n", ROOT);
    printf("Everything downloaded is checked against the SHA-256 in the\n");
    printf("index. The index itself comes over plain HTTP, because there\n");
    printf("is no TLS in this userland: that catches corruption and a bad\n");
    printf("mirror, not somebody who can rewrite your traffic. When that\n");
    printf("matters, fetch the file with python3 over HTTPS, check it\n");
    printf("yourself, and use 'pkg add'.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0)
        return cmd_list();

    if (strcmp(cmd, "info") == 0 && argc > 2)
        return cmd_info(argv[2]);

    if (strcmp(cmd, "add") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_add(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "remove") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_remove(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "repo") == 0)
        return cmd_repo(argc > 2 ? argv[2] : NULL);

    if (strcmp(cmd, "update") == 0)
        return cmd_update();

    if (strcmp(cmd, "search") == 0)
        return cmd_search(argc > 2 ? argv[2] : NULL);

    if (strcmp(cmd, "install") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_install(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "sha256") == 0 && argc > 2) {
        char hex[72];
        if (!sha256_file(argv[2], hex)) {
            dprintf(STDERR_FILENO, "pkg: %s: cannot read\n", argv[2]);
            return 1;
        }
        printf("%s  %s\n", hex, argv[2]);
        return 0;
    }

    usage();
    return 2;
}
