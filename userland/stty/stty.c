/* stty - the terminal's size, and putting it back to sane.
 *
 *   stty                    rows and columns, as the kernel has them
 *   stty size               "ROWS COLS", for scripts
 *   stty cols N rows N      set them (either, in any order; columns too)
 *   stty sane               echo, line editing and Ctrl-C back on
 *
 * ── Why the size has to be told ──
 *
 * A serial line carries characters and nothing else. On a screen or over
 * SSH the size arrives by itself - the console knows its font and
 * resolution, and SSH sends the window size along - but a serial
 * console's size is whatever somebody sets, and nobody does: the kernel
 * leaves it at 0x0, and every full-screen program then guesses 80x24.
 * In a terminal window that is narrower than that (a phone) or bigger
 * (anything else), the editor wraps its own lines and `top` cuts off.
 *
 * UTM's terminal - the way to reach this system in a virtual machine on
 * an iPhone or iPad - knows its size and sends exactly this line when
 * asked to fit the window:
 *
 *     stty cols 54 rows 38
 *
 * With no stty here that was "command not found" on the screen every
 * time. Now it sets the size the editor, `top`, `more`, and whatever
 * apt installs (nano, htop, vim) all read (TIOCGWINSZ).
 *
 * The rest of stty's settings - baud rate, parity, a hundred flags - mean
 * nothing on this system's consoles, so they are not here; `stty sane`
 * is, for the terminal a crashed full-screen program left without echo.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

/* struct winsize { u16 row, col, xpixel, ypixel; } */
static int get_size(u16 ws[4])
{
    ws[0] = ws[1] = ws[2] = ws[3] = 0;
    return lp_ioctl(STDIN_FILENO, TIOCGWINSZ, ws) < 0 ? -1 : 0;
}

/* A size from the command line: a whole number, 1 to 9999. */
static int parse_size(const char *s, u16 *out)
{
    if (!s || !*s)
        return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
        if (v > 9999)
            return -1;
    }
    if (v < 1)
        return -1;
    *out = (u16)v;
    return 0;
}

static void usage(void)
{
    printf("Usage: stty [size | sane | cols N | rows N]...\n"
           "The terminal's size, and resetting it.\n\n"
           "  stty                 print the rows and columns\n"
           "  stty size            print \"ROWS COLS\"\n"
           "  stty cols N rows N   set the size (columns is the same as cols)\n"
           "  stty sane            echo, line editing and Ctrl-C back on\n\n"
           "A serial console does not know its size; UTM's terminal sends\n"
           "`stty cols N rows N` when you ask it to fit the window.\n");
}

int main(int argc, char **argv)
{
    u16 ws[4];

    if (!lp_isatty(STDIN_FILENO)) {
        dprintf(STDERR_FILENO, "stty: standard input is not a terminal\n");
        return 1;
    }
    if (get_size(ws) < 0) {
        dprintf(STDERR_FILENO, "stty: cannot read the terminal's size\n");
        return 1;
    }
    if (argc == 1 || (argc == 2 && (strcmp(argv[1], "-a") == 0 ||
                                    strcmp(argv[1], "--all") == 0))) {
        printf("rows %u; columns %u;%s\n", ws[0], ws[1],
               ws[0] == 0 ? "  (not set - `stty cols N rows N`)" : "");
        return 0;
    }

    bool set = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "size") == 0) {
            printf("%u %u\n", ws[0], ws[1]);
        } else if (strcmp(a, "sane") == 0) {
            if (lp_term_sane(STDIN_FILENO) < 0) {
                dprintf(STDERR_FILENO, "stty: cannot change the terminal's settings\n");
                return 1;
            }
        } else if (strcmp(a, "cols") == 0 || strcmp(a, "columns") == 0 ||
                   strcmp(a, "rows") == 0) {
            u16 v;
            if (i + 1 >= argc || parse_size(argv[i + 1], &v) < 0) {
                dprintf(STDERR_FILENO, "stty: %s needs a number from 1 to 9999\n", a);
                return 1;
            }
            if (a[0] == 'r')
                ws[0] = v;
            else
                ws[1] = v;
            set = true;
            i++;
        } else {
            dprintf(STDERR_FILENO, "stty: %s: not a setting this stty has (size, sane, cols, rows)\n", a);
            return 1;
        }
    }
    if (set && lp_ioctl(STDIN_FILENO, TIOCSWINSZ, ws) < 0) {
        dprintf(STDERR_FILENO, "stty: cannot set the terminal's size\n");
        return 1;
    }
    return 0;
}
