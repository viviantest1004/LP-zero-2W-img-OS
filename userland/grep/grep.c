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
} opts_t;

/* Returns the number of matching lines. */
static long grep_fd(int fd, const char *pat, const opts_t *o,
                    const char *name, bool show_name)
{
    char line[8192];
    long lineno = 0, hits = 0;

    while (readline(fd, line, sizeof(line)) >= 0) {
        lineno++;
        bool hit = match_line(pat, line);
        if (o->invert)
            hit = !hit;
        if (!hit)
            continue;

        hits++;
        if (o->quiet || o->count_only)
            continue;
        if (o->names_only) {
            printf("%s\n", name);
            break;                    /* one mention per file is enough */
        }

        if (show_name)
            printf("%s:", name);
        if (o->numbers)
            printf("%ld:", lineno);
        printf("%s\n", line);
    }
    return hits;
}

int main(int argc, char **argv)
{
    opts_t o = { false, false, false, false, false };
    const char *pat = NULL;
    int files = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !pat) {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'i': fold = true; break;
                case 'v': o.invert = true; break;
                case 'n': o.numbers = true; break;
                case 'c': o.count_only = true; break;
                case 'l': o.names_only = true; break;
                case 'q': o.quiet = true; break;
                case 'h':
                    printf("usage: grep [-invclq] <pattern> [file]...\n");
                    printf("  -i ignore case   -v show the lines that do not match\n");
                    printf("  -n line numbers  -c count only\n");
                    printf("  -l name the files  -q say nothing, just the exit code\n");
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
        dprintf(STDERR_FILENO, "usage: grep [-invclq] <pattern> [file]...\n");
        return 2;
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
