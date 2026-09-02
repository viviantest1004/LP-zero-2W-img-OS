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

typedef struct { long lines, words, chars; } count_t;

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

static void report(const count_t *c, bool l, bool w, bool ch, const char *name)
{
    if (l)  printf("%7ld", c->lines);
    if (w)  printf("%7ld", c->words);
    if (ch) printf("%7ld", c->chars);
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
        report(&c, l, w, ch, NULL);
        return 0;
    }

    count_t total = { 0, 0, 0 };
    int rc = 0, seen = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1])
            continue;

        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        count_t c = { 0, 0, 0 };
        count_fd((int)fd, &c);
        lp_close((int)fd);

        report(&c, l, w, ch, argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.chars += c.chars;
        seen++;
    }

    if (seen > 1)
        report(&total, l, w, ch, "total");
    return rc;
}
