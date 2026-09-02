/* sort - put lines in order.
 *
 *   sort [-r] [-n] [-u] [file]...
 *
 *   -r  reverse   -n  compare as numbers   -u  drop duplicates
 *
 * Everything is read into memory first, because a line cannot be placed
 * until every other line has been seen. That bounds what this can sort
 * to what fits in RAM, which on this board is the right trade: a sort
 * that spills to the SD card would wear it out for a case that does not
 * come up.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_LINES 20000

static char *lines[MAX_LINES];
static int   nlines;

static bool numeric, reverse, unique;

static int compare(const char *a, const char *b)
{
    if (numeric) {
        long x = strtol(a, NULL, 10);
        long y = strtol(b, NULL, 10);
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }
    return strcmp(a, b);
}

static void read_fd(int fd)
{
    char line[4096];
    while (nlines < MAX_LINES) {
        long n = readline(fd, line, sizeof(line));
        if (n < 0)
            break;
        char *copy = malloc(strlen(line) + 1);
        if (!copy)
            break;
        strcpy(copy, line);
        lines[nlines++] = copy;
    }
}

/* Merge sort: n log n without the worst case a quicksort can hit on
 * input that is already in order, which is exactly the input this gets
 * given most often. */
static void merge_sort(char **a, int n, char **tmp)
{
    if (n < 2)
        return;

    int mid = n / 2;
    merge_sort(a, mid, tmp);
    merge_sort(a + mid, n - mid, tmp);

    int i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        int c = compare(a[i], a[j]);
        if (reverse) c = -c;
        tmp[k++] = (c <= 0) ? a[i++] : a[j++];
    }
    while (i < mid) tmp[k++] = a[i++];
    while (j < n)   tmp[k++] = a[j++];

    for (int m = 0; m < n; m++)
        a[m] = tmp[m];
}

int main(int argc, char **argv)
{
    int files = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *o = argv[i] + 1; *o; o++) {
                switch (*o) {
                case 'r': reverse = true; break;
                case 'n': numeric = true; break;
                case 'u': unique  = true; break;
                case 'h':
                    printf("usage: sort [-r] [-n] [-u] [file]...\n");
                    printf("  -r reverse   -n numeric   -u drop duplicates\n");
                    return 0;
                default:
                    dprintf(STDERR_FILENO, "sort: unknown option -%c\n", *o);
                    return 2;
                }
            }
        } else {
            files++;
        }
    }

    if (files == 0) {
        read_fd(STDIN_FILENO);
    } else {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1])
                continue;
            long fd = lp_open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                dprintf(STDERR_FILENO, "sort: %s: cannot open\n", argv[i]);
                continue;
            }
            read_fd((int)fd);
            lp_close((int)fd);
        }
    }

    if (nlines == 0)
        return 0;

    char **tmp = malloc(sizeof(char *) * (size_t)nlines);
    if (!tmp) {
        dprintf(STDERR_FILENO, "sort: out of memory (%d lines)\n", nlines);
        return 1;
    }
    merge_sort(lines, nlines, tmp);

    for (int i = 0; i < nlines; i++) {
        if (unique && i > 0 && strcmp(lines[i], lines[i - 1]) == 0)
            continue;
        printf("%s\n", lines[i]);
    }
    return 0;
}
