/* cut - take columns out of each line.
 *
 *   cut -f 1,3 [-d :] [file]     fields, split on a delimiter (tab by default)
 *   cut -c 1-10 [file]           characters
 *
 * Field numbers start at 1, and a range is written 2-5. Missing fields
 * produce nothing rather than an error: lines in real files are ragged,
 * and stopping on the first short one is rarely what anyone wants.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_RANGES 16

typedef struct { int from, to; } range_t;
static range_t ranges[MAX_RANGES];
static int nranges;

static bool wanted(int n)
{
    for (int i = 0; i < nranges; i++)
        if (n >= ranges[i].from && (ranges[i].to == 0 || n <= ranges[i].to))
            return true;
    return false;
}

/* "1,3,5-7" or "2-" */
static bool parse_list(const char *spec)
{
    while (*spec && nranges < MAX_RANGES) {
        int from = (int)strtol(spec, (char **)&spec, 10);
        int to   = from;

        if (*spec == '-') {
            spec++;
            if (*spec >= '0' && *spec <= '9')
                to = (int)strtol(spec, (char **)&spec, 10);
            else
                to = 0;              /* open ended */
        }
        if (from < 1)
            return false;

        ranges[nranges].from = from;
        ranges[nranges].to   = to;
        nranges++;

        if (*spec == ',') spec++;
        else if (*spec)   return false;
    }
    return nranges > 0;
}

static void cut_fields(const char *line, char delim)
{
    int  field = 1;
    bool first = true;
    const char *p = line;

    while (p) {
        const char *end = strchr(p, delim);
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (wanted(field)) {
            if (!first) printf("%c", delim);
            lp_write(STDOUT_FILENO, p, len);
            first = false;
        }

        field++;
        p = end ? end + 1 : NULL;
    }
    printf("\n");
}

static void cut_chars(const char *line)
{
    /* By character, not by byte: cutting a Hangul syllable in half
     * produces something that is not text at all. */
    int col = 1;
    size_t i = 0, len = strlen(line);

    while (i < len) {
        size_t next = utf8_next(line, len, i);
        if (wanted(col))
            lp_write(STDOUT_FILENO, line + i, next - i);
        i = next;
        col++;
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    char delim = '\t';
    bool by_char = false;
    bool have_list = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            have_list = parse_list(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            by_char = true;
            have_list = parse_list(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: cut -f <list> [-d char] [file]\n");
            printf("       cut -c <list> [file]\n");
            printf("  list: 1,3 or 2-5 or 3- (counting from 1)\n");
            printf("  -d  the field separator (a tab by default)\n");
            return 0;
        } else if (!file) {
            file = argv[i];
        }
    }

    if (!have_list) {
        dprintf(STDERR_FILENO, "cut: say which fields: -f 1,3  or  -c 1-10\n");
        return 2;
    }

    int fd = STDIN_FILENO;
    if (file) {
        long f = lp_open(file, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "cut: %s: cannot open\n", file);
            return 1;
        }
        fd = (int)f;
    }

    char line[8192];
    while (readline(fd, line, sizeof(line)) >= 0) {
        if (by_char) cut_chars(line);
        else         cut_fields(line, delim);
    }

    if (fd != STDIN_FILENO)
        lp_close(fd);
    return 0;
}
