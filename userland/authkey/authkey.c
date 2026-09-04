/* authkey - make sure there is still a way in.
 *
 *   authkey            merge the keys and report
 *   authkey -l         list what is authorized
 *   authkey -q         quiet; the exit code is the answer
 *
 * Exits non-zero when no key is authorized, which means nobody can log
 * in - including whoever owns the machine.
 *
 * ── The problem this exists for ──
 * SSH is how this board is used. The key that allows it lives in
 * /root/.ssh/authorized_keys, and /root is a bind mount from /data - so
 * anything that destroys the data partition, ransomware or a mistake or
 * a dying card, takes the key with it. The machine then boots perfectly
 * and is unreachable forever.
 *
 * Root on the machine can reach anything the machine can reach, so this
 * is not about preventing that. It is about making sure that one reboot
 * always gets you back in. Three places hold a copy, and they are
 * progressively harder to destroy:
 *
 *   /data/root/.ssh/authorized_keys   the live one. Assume it is gone.
 *   /boot/authorized_keys             FAT, mounted read-only, editable
 *                                     from any PC with a card reader.
 *                                     Root can still remount and wipe
 *                                     it, but nothing else can.
 *   /etc/authorized_keys              inside the kernel image. Unpacked
 *                                     into RAM at every boot, read-only,
 *                                     and not reachable from any
 *                                     filesystem. Short of writing a new
 *                                     card, this one cannot be removed.
 *
 * Every boot, the outer two are merged into the live one. A key already
 * there is left alone, so keys added by hand on the machine survive, and
 * nothing is duplicated.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LIVE   "/root/.ssh/authorized_keys"
#define BOOT   "/boot/authorized_keys"
#define IMAGE  "/etc/authorized_keys"

#define MAX_KEYS 32
#define KEY_MAX  1024

static char keys[MAX_KEYS][KEY_MAX];
static int  nkeys;

/* A line is a key if it is not blank and not a comment. No parsing of
 * the key itself: dropbear decides what is valid, and a line we did not
 * understand is one we should not be throwing away. */
static bool is_key_line(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    return *line && *line != '#';
}

/* The part that identifies a key, ignoring the trailing comment people
 * put their email address in - so the same key added twice under two
 * different names is still the same key. */
static void key_body(const char *line, char *out, size_t size)
{
    while (*line == ' ' || *line == '\t')
        line++;

    /* type SPACE base64 SPACE comment */
    const char *sp = strchr(line, ' ');
    if (!sp) {
        strlcpy(out, line, size);
        return;
    }
    const char *body = sp + 1;
    while (*body == ' ') body++;

    const char *end = strchr(body, ' ');
    size_t len = end ? (size_t)(end - body) : strlen(body);
    if (len >= size) len = size - 1;
    memcpy(out, body, len);
    out[len] = '\0';
}

static bool already_have(const char *line)
{
    char body[KEY_MAX], other[KEY_MAX];
    key_body(line, body, sizeof(body));

    for (int i = 0; i < nkeys; i++) {
        key_body(keys[i], other, sizeof(other));
        if (strcmp(body, other) == 0)
            return true;
    }
    return false;
}

/* Returns how many new keys came out of this file. */
static int load(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char line[KEY_MAX];
    int  added = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (!is_key_line(line))
            continue;
        if (nkeys >= MAX_KEYS) {
            dprintf(STDERR_FILENO,
                    "authkey: more than %d keys - the rest of %s is"
                    " ignored\n", MAX_KEYS, path);
            break;
        }
        if (already_have(line))
            continue;
        strlcpy(keys[nkeys++], line, KEY_MAX);
        added++;
    }

    lp_close((int)fd);
    return added;
}

static bool write_live(void)
{
    /* The directory may not be there on a boot where /data did not
     * mount: /root is then whatever the image has. */
    lp_mkdir("/root", 0700);
    lp_mkdir("/root/.ssh", 0700);

    long fd = lp_open(LIVE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;

    for (int i = 0; i < nkeys; i++) {
        lp_write((int)fd, keys[i], strlen(keys[i]));
        lp_write((int)fd, "\n", 1);
    }
    lp_close((int)fd);
    return true;
}

int main(int argc, char **argv)
{
    bool quiet = false, list_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = true;
        else if (strcmp(argv[i], "-l") == 0) list_only = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: authkey [-l] [-q]\n");
            printf("  Merges the SSH keys from the boot partition and\n");
            printf("  from the system image into %s,\n", LIVE);
            printf("  so that losing the data partition does not lock\n");
            printf("  you out of your own machine.\n\n");
            printf("  -l  list what is authorized, and where it came from\n");
            printf("  -q  say nothing; the exit code is the answer\n\n");
            printf("  Exit 1 means no key is authorized - nobody can\n");
            printf("  log in over SSH, including you.\n");
            return 0;
        }
    }

    /* ── Recovery keys first, live keys after ──
     *
     * This used to be live, boot, image - and the table stops at
     * MAX_KEYS. The live file is on /data, which this program's own
     * header says to treat as lost or hostile, so anyone who could
     * write it could pad it to MAX_KEYS lines and the image key - the
     * one described here as impossible to remove without writing a new
     * card - was silently the first thing dropped. authkey then
     * reported "32 keys" and exited 0, and the documented recovery
     * route (put your key on the FAT partition and reboot) stopped
     * working, permanently, on a board with no Ethernet and no
     * passwords.
     *
     * The two keys that exist to get you back in are now loaded first
     * and cannot be crowded out. */
    int image = load(IMAGE);
    int boot  = load(BOOT);
    int live  = load(LIVE);

    if (list_only) {
        if (nkeys == 0) {
            printf("no keys authorized - nobody can log in over SSH\n");
            return 1;
        }
        printf("%d key%s authorized:\n", nkeys, nkeys == 1 ? "" : "s");
        for (int i = 0; i < nkeys; i++) {
            /* The tail of the line is usually who it belongs to. */
            printf("  %s\n", keys[i]);
        }
        return 0;
    }

    if (nkeys == 0) {
        if (!quiet) {
            dprintf(STDERR_FILENO,
                "authkey: ** no SSH key anywhere. Nobody can log in.\n"
                "authkey:    Put your public key in authorized_keys on the\n"
                "authkey:    boot partition - it is FAT32, so any PC can\n"
                "authkey:    write it - and reboot.\n");
        }
        return 1;
    }

    /* Only write when something changed, so a healthy boot does not
     * touch the data partition at all. */
    if (boot > 0 || image > 0) {
        if (!write_live()) {
            if (!quiet)
                dprintf(STDERR_FILENO,
                        "authkey: cannot write %s\n"
                        "authkey:   the keys are in RAM for this boot only\n",
                        LIVE);
            return 0;           /* the keys are still usable this boot */
        }
        if (!quiet)
            printf("authkey: %d key%s (%d recovered from the boot partition,"
                   " %d from the system image)\n",
                   nkeys, nkeys == 1 ? "" : "s", boot, image);
    } else if (!quiet) {
        printf("authkey: %d key%s\n", live, live == 1 ? "" : "s");
    }

    return 0;
}
