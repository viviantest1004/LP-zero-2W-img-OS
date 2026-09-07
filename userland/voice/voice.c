/* voice - choose the wording the commands use.
 *
 *   voice           which one is set, with an example
 *   voice gnu       GNU's wording, exactly as Ubuntu says it (default)
 *   voice lp        this system's own, which names the errno
 *
 * ── Why there is a choice at all ──
 *
 * Every command here was written from scratch, and each invented its own
 * phrasing for "that file is not there". They were clear and they were
 * all different:
 *
 *     ours   cat: nosuch: cannot open (2)
 *     GNU    cat: nosuch: No such file or directory
 *
 * On a machine somebody is learning Linux on, different is worse than
 * missing. A missing command teaches nothing. A command that answers
 * differently teaches something false, and it is not found out here - it
 * is found out on Ubuntu, months later, when the thing they were sure
 * of turns out to be true only of this machine.
 *
 * So GNU's wording is the default and it is exact, down to the quotes.
 *
 * The other voice is kept rather than deleted because it is genuinely
 * better at one thing: it prints the errno. When a write fails, "(28)"
 * sends you straight to ENOSPC, and "No space left on device" only tells
 * you what you already suspected. Somebody past the learning stage may
 * want that back, and this is how.
 *
 * The setting lives in /data/voice so it survives a reboot on a machine
 * whose root is in RAM, and /etc/voice is the fallback an image can ship.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LIVE  "/data/voice"
#define IMAGE "/etc/voice"

static void show(void)
{
    bool gnu = lp_voice() == LP_VOICE_GNU;
    printf("voice: %s\n\n", gnu ? "gnu" : "lp");
    printf("  a missing file reads as\n");
    if (gnu)
        printf("    cat: nosuch: No such file or directory\n");
    else
        printf("    cat: nosuch: cannot open (2)\n");
    printf("\n");
    printf("  voice gnu    GNU's wording, as Ubuntu says it\n");
    printf("  voice lp     this system's own, with the errno number\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        show();
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printf("usage: voice [gnu|lp]\n");
        printf("  gnu   GNU's wording, exactly (the default)\n");
        printf("  lp    this system's own, which names the errno\n");
        return 0;
    }

    const char *want;
    if      (strcmp(argv[1], "gnu") == 0) want = "gnu";
    else if (strcmp(argv[1], "lp")  == 0) want = "lp";
    else {
        dprintf(STDERR_FILENO,
                "voice: %s? it is 'gnu' or 'lp'\n", argv[1]);
        return 2;
    }

    /* /data first, because that is the half that survives a reboot.
     * Falling back to /etc is not silent: a setting that only lasts
     * until the power goes is worth knowing about. */
    const char *path = LIVE;
    long fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        path = IMAGE;
        fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            lp_diag("voice", "cannot write", NULL, "cannot write", LIVE, (int)-fd);
            return 1;
        }
        dprintf(STDERR_FILENO,
                "voice: /data is not writable, so this went to %s and\n"
                "voice:   will be lost at the next reboot.\n", IMAGE);
    }
    lp_write((int)fd, want, strlen(want));
    lp_write((int)fd, "\n", 1);
    lp_close((int)fd);
    lp_sync();

    /* Read it back through the same door every other command uses, so
     * what is printed now is what they will actually do. */
    show();
    return 0;
}
