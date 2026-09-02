/* kill - stop a process.
 *
 *   kill <pid>...          ask it to stop (TERM)
 *   kill -9 <pid>...       force it (KILL)
 *   kill -l                list the signals we know
 *
 * TERM asks; a program can catch it, save its work and exit. KILL
 * cannot be caught, so anything unwritten is lost. Try TERM first.
 *
 * top can do this interactively - this is the same thing for scripts.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

typedef struct { int num; const char *name; const char *what; } sig_t;

static const sig_t SIGNALS[] = {
    {  1, "HUP",  "hung up - many daemons reread their config" },
    {  2, "INT",  "interrupt, same as Ctrl-C" },
    {  9, "KILL", "cannot be caught or ignored" },
    { 15, "TERM", "please stop (the default)" },
    { 18, "CONT", "continue a stopped process" },
    { 19, "STOP", "pause it, cannot be caught" },
    {  0, NULL, NULL }
};

static int signal_by_name(const char *s)
{
    if (*s >= '0' && *s <= '9')
        return atoi(s);
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (int i = 0; SIGNALS[i].name; i++)
        if (strcmp(SIGNALS[i].name, s) == 0)
            return SIGNALS[i].num;
    return -1;
}

int main(int argc, char **argv)
{
    int sig   = 15;                 /* TERM */
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        printf("signals:\n");
        for (int i = 0; SIGNALS[i].name; i++)
            printf("  %2d  %-5s %s\n",
                   SIGNALS[i].num, SIGNALS[i].name, SIGNALS[i].what);
        return 0;
    }

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        sig = signal_by_name(argv[1] + 1);
        if (sig < 0) {
            dprintf(STDERR_FILENO,
                    "kill: unknown signal: %s   (kill -l lists them)\n",
                    argv[1] + 1);
            return 2;
        }
        first = 2;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO,
                "usage: kill [-signal] <pid>...\n"
                "       kill -l          list signals\n");
        return 2;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            dprintf(STDERR_FILENO, "kill: not a pid: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        /* Signalling pid 1 takes the machine down. reboot and poweroff
         * are the ways to do that on purpose. */
        if (pid == 1) {
            dprintf(STDERR_FILENO,
                    "kill: refusing pid 1 (init). Use reboot or poweroff.\n");
            rc = 1;
            continue;
        }
        long r = lp_kill(pid, sig);
        if (r < 0) {
            dprintf(STDERR_FILENO, "kill: %d: %s (%ld)\n", pid,
                    -r == 3 ? "no such process" :
                    -r == 1 ? "not permitted" : "failed", -r);
            rc = 1;
        }
    }
    return rc;
}
