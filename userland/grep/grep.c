/* grep - print the lines that match.
 *
 *   grep [-i] [-v] [-n] [-c] [-l] [-q] <pattern> [file]...
 *
 * The pattern is a regular expression, but a small one:
 *
 *   .        any single character
 *   *        none or more of what came before
 *   +        one or more
 *   ?        none or one
 *   [abc]    any of these, [a-z] a range, [^a] anything but
 *   ^ $      the start and the end of the line
 *
 * No groups, no alternation, no backreferences. Those need a compiled
 * automaton and a stack, and this fits in a page of code by walking the
 * pattern and the text together. Everything else is a literal character,
 * so an ordinary search still reads like an ordinary search.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool fold;                     /* -i */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Does one pattern item match this character? Returns the item's length
 * in the pattern, or 0 when the item itself is malformed. */
static int item_len(const char *p)
{
    if (*p == '[') {
        const char *q = p + 1;
        if (*q == '^') q++;
        if (*q == ']') q++;           /* a ] right after [ is a literal */
        while (*q && *q != ']') q++;
        return *q == ']' ? (int)(q - p) + 1 : 0;
    }
    if (*p == '\\' && p[1])
        return 2;
    return *p ? 1 : 0;
}

static bool item_match(const char *p, char c)
{
    if (*p == '[') {
        const char *q = p + 1;
        bool negate = false;
        if (*q == '^') { negate = true; q++; }

        bool hit = false;
        while (*q && *q != ']') {
            if (q[1] == '-' && q[2] && q[2] != ']') {
                char lo = fold ? lower(q[0]) : q[0];
                char hi = fold ? lower(q[2]) : q[2];
                char ch = fold ? lower(c) : c;
                if (ch >= lo && ch <= hi) hit = true;
                q += 3;
            } else {
                char ch = fold ? lower(c) : c;
                char pc = fold ? lower(*q) : *q;
                if (ch == pc) hit = true;
                q++;
            }
        }
        return negate ? !hit : hit;
    }

    if (*p == '\\' && p[1])
        return fold ? lower(p[1]) == lower(c) : p[1] == c;

    if (*p == '.')
        return true;

    return fold ? lower(*p) == lower(c) : *p == c;
}

/* Match the pattern against the text from here on. */
static bool match_here(const char *pat, const char *text)
{
    for (;;) {
        if (*pat == '\0')
            return true;
        if (*pat == '$' && pat[1] == '\0')
            return *text == '\0';

        int ilen = item_len(pat);
        if (ilen == 0)
            return false;

        char rep = pat[ilen];
        if (rep == '*' || rep == '+' || rep == '?') {
            const char *rest = pat + ilen + 1;

            /* + has to match once before the loop below takes over. */
            if (rep == '+') {
                if (!*text || !item_match(pat, *text))
                    return false;
                text++;
            }

            if (rep == '?') {
                if (*text && item_match(pat, *text) && match_here(rest, text + 1))
                    return true;
                pat = rest;
                continue;
            }

            /* Greedy, but backing off one character at a time: run
             * forward as far as the item matches, then try the rest of
             * the pattern from the furthest point backwards. */
            const char *end = text;
            while (*end && item_match(pat, *end))
                end++;
            for (const char *t = end; t >= text; t--)
                if (match_here(rest, t))
                    return true;
            return false;
        }

        if (!*text || !item_match(pat, *text))
            return false;
        pat += ilen;
        text++;
    }
}

static bool match_line(const char *pat, const char *line)
{
    if (*pat == '^')
        return match_here(pat + 1, line);

    /* Unanchored: try every starting point. */
    for (const char *t = line; ; t++) {
        if (match_here(pat, t))
            return true;
        if (!*t)
            return false;
    }
}

typedef struct {
    bool invert, numbers, count_only, names_only, quiet;
    int  before;        /* -B: lines of context before a match */
    int  after;         /* -A: lines after */
} opts_t;

/* Returns the number of matching lines. */
/* ── Context lines: -A, -B, -C ──
 *
 * A match on its own is often not the answer - the answer is the line
 * after it, or the three before. Without these, reading a log meant
 * grepping for a line number and then paging to it by hand, and every
 * script that wanted context had to be written some other way.
 *
 * "Before" needs the lines kept until we know whether they matter, so
 * there is a small ring of them. "After" is only a countdown. */
#define CTX_MAX 32

static void emit(const char *name, bool show_name, long lineno,
                 const opts_t *o, const char *text, char sep)
{
    if (show_name)
        printf("%s%c", name, sep);
    if (o->numbers)
        printf("%ld%c", lineno, sep);
    printf("%s\n", text);
}

static long grep_fd(int fd, const char *pat, const opts_t *o,
                    const char *name, bool show_name)
{
    char line[8192];
    long lineno = 0, hits = 0;

    /* The ring of lines waiting to be shown if a match turns up. */
    static char ring[CTX_MAX][8192];
    static long ring_no[CTX_MAX];
    int  ring_n = 0, ring_head = 0;
    int  before = o->before > CTX_MAX ? CTX_MAX : o->before;

    int  after_left = 0;        /* lines still owed after a match */
    long last_shown = 0;        /* to place --  separators */
    bool any_output = false;

    while (readline(fd, line, sizeof(line)) >= 0) {
        lineno++;
        bool hit = match_line(pat, line);
        if (o->invert)
            hit = !hit;

        if (!hit) {
            if (after_left > 0 && !o->quiet && !o->count_only &&
                !o->names_only) {
                emit(name, show_name, lineno, o, line, '-');
                last_shown = lineno;
                after_left--;
            } else if (before > 0) {
                strlcpy(ring[ring_head], line, sizeof ring[0]);
                ring_no[ring_head] = lineno;
                ring_head = (ring_head + 1) % before;
                if (ring_n < before)
                    ring_n++;
            }
            continue;
        }

        hits++;
        if (o->quiet || o->count_only)
            continue;
        if (o->names_only) {
            printf("%s\n", name);
            break;                    /* one mention per file is enough */
        }

        /* A gap between groups gets a separator, the way grep does it.
         * Without it two runs of context read as one continuous quote
         * from the file, which is exactly the wrong impression. */
        if ((o->before || o->after) && any_output &&
            last_shown && lineno > last_shown + 1)
            printf("--\n");

        for (int i = 0; i < ring_n; i++) {
            int idx = (ring_head + before - ring_n + i) % before;
            if (ring_no[idx] > last_shown)
                emit(name, show_name, ring_no[idx], o, ring[idx], '-');
        }
        ring_n = 0;

        emit(name, show_name, lineno, o, line, ':');
        last_shown = lineno;
        any_output = true;
        after_left = o->after;
    }
    return hits;
}

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR         4
#define DT_REG         8

static bool recursive = false;
static long recurse_total = 0;
static bool any_match = false;

/* Walk a directory and grep every ordinary file under it.
 *
 * Depth is capped rather than unbounded: a symlink pointing at its own
 * parent makes an infinite tree, and a grep that never returns on a
 * machine reached only over SSH is worse than one that stops early and
 * says so. */
static void grep_tree(const char *path, const char *pat, opts_t *o, int depth)
{
    if (depth > 24) {
        dprintf(STDERR_FILENO,
                "grep: %s is more than 24 directories deep - stopping.\n"
                "grep:   A symlink pointing at its own parent looks like\n"
                "grep:   this.\n", path);
        return;
    }

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        /* Not a directory: it is a file to search. */
        long f = lp_open(path, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "grep: %s: cannot open\n", path);
            return;
        }
        long n = grep_fd((int)f, pat, o, path, true);
        lp_close((int)f);
        recurse_total += n;
        if (n > 0) any_match = true;
        return;
    }

    /* The names are collected before recursing: getdents' buffer is
     * reused by the directory below. */
    static char names[512][256];
    static u8   types[512];
    int n = 0;
    char buf[4096];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && n < 512; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] == '.') continue;       /* and . and .. with it */
            strlcpy(names[n], name, 256);
            types[n] = type;
            n++;
        }
        if (n >= 512) break;
    }
    lp_close((int)fd);

    for (int i = 0; i < n; i++) {
        char child[768];
        snprintf(child, sizeof child, "%s/%s", path, names[i]);
        if (types[i] == DT_DIR)
            grep_tree(child, pat, o, depth + 1);
        else if (types[i] == DT_REG || types[i] == 0)
            grep_tree(child, pat, o, depth + 1);
    }
}

int main(int argc, char **argv)
{
    opts_t o = { false, false, false, false, false, 0, 0 };
    const char *pat = NULL;
    int files = 0;

    for (int i = 1; i < argc; i++) {
        if (!argv[i][0])
            continue;                     /* a consumed option value */

        /* -A 3 / -B 3 / -C 3, and the attached form -A3. These take a
         * value, so they cannot be folded in with the flag letters. */
        if (argv[i][0] == '-' && !pat &&
            (argv[i][1] == 'A' || argv[i][1] == 'B' || argv[i][1] == 'C')) {
            char which = argv[i][1];
            const char *val = argv[i][2] ? argv[i] + 2
                                         : (i + 1 < argc ? argv[++i] : NULL);
            if (!val || val[0] < '0' || val[0] > '9') {
                dprintf(STDERR_FILENO,
                        "grep: -%c wants a number of lines\n", which);
                return 2;
            }
            int n = atoi(val);
            if (n < 0) n = 0;
            if (which == 'A' || which == 'C') o.after  = n;
            if (which == 'B' || which == 'C') o.before = n;

            /* Mark the value as consumed.
             *
             * Three later loops walk argv again to find the pattern and
             * the files, and they decide by "does it start with -".
             * The number after -A does not, so `grep -A 2 foo file`
             * looked for a file called "2" and said "2: cannot open"
             * while still doing the right thing with the real file -
             * the sort of half-working that is worse than a clean
             * failure. Blanking it here means every one of those loops
             * skips it without having to know the option exists. */
            argv[i] = (char *)"";
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] && !pat) {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'i': fold = true; break;
                case 'v': o.invert = true; break;
                case 'n': o.numbers = true; break;
                case 'c': o.count_only = true; break;
                case 'l': o.names_only = true; break;
                case 'q': o.quiet = true; break;
                case 'r': case 'R': recursive = true; break;
                case 'h':
                    printf("usage: grep [-invclqr] [-A n] [-B n] [-C n] <pattern> [file|dir]...\n");
                    printf("  -i ignore case   -v show the lines that do not match\n");
                    printf("  -n line numbers  -c count only\n");
                    printf("  -l name the files  -q say nothing, just the exit code\n");
                    printf("  -r walk into directories\n");
                    printf("  -A n lines after a match   -B n before   -C n both\n");
                    printf("\npattern: . any character, * + ? repeats,\n");
                    printf("         [abc] [a-z] [^a] sets, ^ $ the line ends\n");
                    return 0;
                default:
                    dprintf(STDERR_FILENO, "grep: unknown option -%c\n", *f);
                    return 2;
                }
            }
            continue;
        }
        if (!pat) pat = argv[i];
        else      files++;
    }

    if (!pat) {
        dprintf(STDERR_FILENO,
                "usage: grep [-invclqr] <pattern> [file|dir]...\n");
        return 2;
    }

    if (recursive) {
        /* With -r and no path, search here - which is what every grep
         * does and what "grep -r foo" alone is always meant to mean. */
        bool searched = false;
        bool seen = false;
        for (int i = 1; i < argc; i++) {
            if (!argv[i][0]) continue;
            if (argv[i][0] == '-' && argv[i][1]) continue;
            if (!seen && strcmp(argv[i], pat) == 0) { seen = true; continue; }
            grep_tree(argv[i], pat, &o, 0);
            searched = true;
        }
        if (!searched)
            grep_tree(".", pat, &o, 0);
        if (o.count_only)
            printf("%ld\n", recurse_total);
        return any_match ? 0 : 1;
    }

    long total = 0;

    if (files == 0) {
        total = grep_fd(STDIN_FILENO, pat, &o, "(stdin)", false);
        if (o.count_only)
            printf("%ld\n", total);
        return total > 0 ? 0 : 1;
    }

    bool seen_pat = false;
    int  rc_any = 1;

    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) continue;
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        if (!seen_pat && strcmp(argv[i], pat) == 0) { seen_pat = true; continue; }

        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "grep: %s: cannot open\n", argv[i]);
            continue;
        }
        long n = grep_fd((int)fd, pat, &o, argv[i], files > 1);
        lp_close((int)fd);

        if (o.count_only)
            printf("%s%ld\n", files > 1 ? argv[i] : "", n);
        if (n > 0)
            rc_any = 0;
        total += n;
    }

    return rc_any;
}
