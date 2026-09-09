/* wc - count lines, words and characters.
 *
 *   wc [-l] [-w] [-c] [file]...
 *
 * With no flags it prints all three. With no file it reads what is piped
 * in, which is the common use: "help | wc -l".
 *
 * Characters, not bytes: a Hangul syllable is three bytes and one
 * character, and counting it as three would be a lie on this system -
 * the whole userland is UTF-8 throughout.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* s64, not long. On a 32-bit machine a long stops at two billion, and
 * `wc -c` on a file bigger than that reported the remainder rather than
 * failing - the one kind of wrong answer that looks like a real one. */
typedef struct { s64 lines, words, chars; } count_t;

static void count_fd(int fd, count_t *c)
{
    char buf[4096];
    bool in_word = false;

    for (;;) {
        long n = lp_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];

            /* Only count the first byte of each character. The rest of a
             * UTF-8 sequence all start with the bits 10. */
            if ((ch & 0xC0) != 0x80)
                c->chars++;

            if (ch == '\n')
                c->lines++;

            bool space = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
            if (space) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                c->words++;
            }
        }
    }
}

/* How wide the number columns are.
 *
 * A fixed %7ld looked tidy and was wrong: `wc -l < f` printed
 * "      3" where every other wc prints "3", so a script doing
 * `n=$(wc -l < f)` got a string with six spaces in front of it. GNU
 * sizes the column to the largest number it is about to print - across
 * every file, so the lines still line up - and that means the counts
 * have to be collected before any of them is printed. */
static int digits(s64 v)
{
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static void report(const count_t *c, bool l, bool w, bool ch,
                   const char *name, int width)
{
    bool first = true;
    if (l)  { printf("%*lld", first ? width : width + 1, (long long)c->lines); first = false; }
    if (w)  { printf("%*lld", first ? width : width + 1, (long long)c->words); first = false; }
    if (ch) { printf("%*lld", first ? width : width + 1, (long long)c->chars); first = false; }
    if (name) printf(" %s", name);
    printf("\n");
}

int main(int argc, char **argv)
{
    bool l = false, w = false, ch = false;
    int  files = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) l = true;
        else if (strcmp(argv[i], "-w") == 0) w = true;
        else if (strcmp(argv[i], "-c") == 0) ch = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: wc [-l] [-w] [-c] [file]...\n");
            printf("  -l lines   -w words   -c characters\n");
            return 0;
        }
        else files++;
    }

    if (!l && !w && !ch)
        l = w = ch = true;

    if (files == 0) {
        count_t c = { 0, 0, 0 };
        count_fd(STDIN_FILENO, &c);
        s64 m = 0;
        if (l && c.lines > m) m = c.lines;
        if (w && c.words > m) m = c.words;
        if (ch && c.chars > m) m = c.chars;
        report(&c, l, w, ch, NULL, digits(m));
        return 0;
    }

    /* Collected first, printed after, so the width is the same on every
     * line - which is the point of having one. */
    static count_t got[256];
    static const char *names[256];
    count_t total = { 0, 0, 0 };
    int rc = 0, seen = 0;

    for (int i = 1; i < argc && seen < 256; i++) {
        if (argv[i][0] == '-' && argv[i][1])
            continue;

        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            lp_diag("wc", NULL, NULL, "cannot open", argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        count_t c = { 0, 0, 0 };
        count_fd((int)fd, &c);
        lp_close((int)fd);

        got[seen]   = c;
        names[seen] = argv[i];
        seen++;
        total.lines += c.lines;
        total.words += c.words;
        total.chars += c.chars;
    }

    s64 m = 0;
    for (int i = 0; i < seen; i++) {
        if (l  && got[i].lines > m) m = got[i].lines;
        if (w  && got[i].words > m) m = got[i].words;
        if (ch && got[i].chars > m) m = got[i].chars;
    }
    if (seen > 1) {
        if (l  && total.lines > m) m = total.lines;
        if (w  && total.words > m) m = total.words;
        if (ch && total.chars > m) m = total.chars;
    }
    int width = digits(m);

    for (int i = 0; i < seen; i++)
        report(&got[i], l, w, ch, names[i], width);
    if (seen > 1)
        report(&total, l, w, ch, "total", width);
    return rc;
}
