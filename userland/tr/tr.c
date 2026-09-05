/* tr - replace, squeeze or delete characters.
 *
 *   tr abc xyz          a->x, b->y, c->z
 *   tr a-z A-Z          ranges
 *   tr -d '\r'          delete them
 *   tr -s ' '           collapse runs of them into one
 *   tr -c a-z .         everything that is NOT in the first set
 *
 * Escapes understood in a set: \n \r \t \\ \0 and \nnn in octal.
 * Classes: [:alpha:] [:digit:] [:alnum:] [:space:] [:upper:] [:lower:]
 * [:punct:].
 *
 * Bytes, not characters. tr has always worked on bytes, and the two
 * things people actually use it for here - stripping the \r out of a
 * file written on Windows and changing case in ASCII - are byte
 * operations. Feeding it a range of Hangul would not do what you meant,
 * so it says so rather than quietly mangling the text.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static bool warned_utf8 = false;

/* Expand a set specification into a list of bytes. */
static int expand(const char *spec, unsigned char *out, int max)
{
    int n = 0;
    const char *p = spec;

    while (*p && n < max) {
        /* [:class:] */
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            if (end) {
                char name[16];
                size_t len = (size_t)(end - (p + 2));
                if (len < sizeof name) {
                    memcpy(name, p + 2, len);
                    name[len] = '\0';
                    for (int c = 0; c < 256 && n < max; c++) {
                        bool in = false;
                        if (strcmp(name, "alpha") == 0)
                            in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                        else if (strcmp(name, "digit") == 0)
                            in = (c >= '0' && c <= '9');
                        else if (strcmp(name, "alnum") == 0)
                            in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                              || (c >= '0' && c <= '9');
                        else if (strcmp(name, "space") == 0)
                            in = (c == ' ' || c == '\t' || c == '\n' ||
                                  c == '\r' || c == '\f' || c == '\v');
                        else if (strcmp(name, "upper") == 0)
                            in = (c >= 'A' && c <= 'Z');
                        else if (strcmp(name, "lower") == 0)
                            in = (c >= 'a' && c <= 'z');
                        else if (strcmp(name, "punct") == 0)
                            in = (c > 32 && c < 127) &&
                                 !((c >= 'a' && c <= 'z') ||
                                   (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'));
                        if (in) out[n++] = (unsigned char)c;
                    }
                    p = end + 2;
                    continue;
                }
            }
        }

        /* one byte, possibly escaped */
        unsigned char c;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'f': c = '\f'; p++; break;
            case 'v': c = '\v'; p++; break;
            case '\\': c = '\\'; p++; break;
            default:
                if (*p >= '0' && *p <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                    c = (unsigned char)v;
                } else c = (unsigned char)*p++;
            }
        } else {
            if ((unsigned char)*p >= 0x80 && !warned_utf8) {
                dprintf(STDERR_FILENO,
                        "tr: this works on bytes, and \"%s\" is not ASCII -\n"
                        "tr:   the result will not be what you meant. Use sed\n"
                        "tr:   for anything beyond ASCII.\n", spec);
                warned_utf8 = true;
            }
            c = (unsigned char)*p++;
        }

        /* a-z */
        if (*p == '-' && p[1] && p[1] != '-') {
            unsigned char hi;
            p++;
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': hi = '\n'; p++; break;
                case 't': hi = '\t'; p++; break;
                case 'r': hi = '\r'; p++; break;
                default:  hi = (unsigned char)*p++;
                }
            } else hi = (unsigned char)*p++;

            if (hi < c) {
                dprintf(STDERR_FILENO,
                        "tr: the range %c-%c runs backwards\n", c, hi);
                return -1;
            }
            for (int v = c; v <= hi && n < max; v++) out[n++] = (unsigned char)v;
            continue;
        }

        out[n++] = c;
    }
    return n;
}

int main(int argc, char **argv)
{
    bool del = false, squeeze = false, complement = false;
    int i = 1;

    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (strcmp(argv[i], "-h") == 0) {
            printf("usage: tr [-cds] set1 [set2]\n");
            printf("  -d  delete set1   -s  squeeze runs   -c  everything but set1\n");
            printf("  tr a-z A-Z   tr -d '\\r'   tr -s ' '\n");
            return 0;
        }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'd') del = true;
            else if (*f == 's') squeeze = true;
            else if (*f == 'c' || *f == 'C') complement = true;
            else {
                dprintf(STDERR_FILENO, "tr: no such option: -%c\n", *f);
                return 2;
            }
        }
    }

    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: tr [-cds] set1 [set2]\n");
        return 2;
    }

    unsigned char s1[512], s2[512];
    int n1 = expand(argv[i], s1, sizeof s1);
    if (n1 < 0) return 2;
    int n2 = 0;
    if (i + 1 < argc) {
        n2 = expand(argv[i + 1], s2, sizeof s2);
        if (n2 < 0) return 2;
    }

    if (!del && !squeeze && n2 == 0) {
        dprintf(STDERR_FILENO,
                "tr: with no second set there is nothing to change to.\n"
                "tr:   `tr -d %s` deletes them; `tr -s %s` collapses runs.\n",
                argv[i], argv[i]);
        return 2;
    }

    /* Build the tables once: 256 entries beats searching a list per byte
     * on a 1GHz core. */
    bool in_set1[256];
    unsigned char map[256];
    for (int c = 0; c < 256; c++) { in_set1[c] = false; map[c] = (unsigned char)c; }
    for (int k = 0; k < n1; k++) in_set1[s1[k]] = true;
    if (complement)
        for (int c = 0; c < 256; c++) in_set1[c] = !in_set1[c];

    if (!del && n2 > 0) {
        /* A short second set repeats its last character, which is what
         * makes `tr a-z x` turn every letter into x. */
        int k = 0;
        for (int c = 0; c < 256; c++) {
            if (!in_set1[c]) continue;
            map[c] = s2[(k < n2) ? k : n2 - 1];
            k++;
        }
    }

    unsigned char in[8192], out[8192];
    int last = -1;
    for (;;) {
        long n = lp_read(STDIN_FILENO, in, sizeof in);
        if (n <= 0) break;
        int o = 0;
        for (long k = 0; k < n; k++) {
            unsigned char c = in[k];
            if (del && in_set1[c]) continue;
            unsigned char v = (!del && in_set1[c]) ? map[c] : c;
            if (squeeze) {
                bool sq = del ? in_set1[v]
                              : (n2 > 0 ? in_set1[c] : in_set1[v]);
                if (sq && v == last) continue;
            }
            out[o++] = v;
            last = v;
        }
        if (o && lp_write(STDOUT_FILENO, out, (size_t)o) < 0) return 1;
    }
    return 0;
}
