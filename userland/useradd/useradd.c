/* useradd - make a user.
 *
 *   useradd [-u uid] [-g gid] [-d home] [-s shell] <name>
 *   userdel <name>
 *   useradd -l                list the users there are
 *
 * ── Where the user is kept ──
 * /etc is in RAM. It is unpacked out of the kernel image at every boot,
 * so a line appended to /etc/passwd is gone by morning - which would
 * make this command look like it worked and quietly undo itself.
 *
 * So a user added here is written to /data/users, and /etc/rc appends
 * that file to /etc/passwd and /etc/group on every boot. Same trick as
 * the SSH keys, for the same reason: the only writeable thing that
 * survives a reboot is the data partition.
 *
 * Delete the file and the machine is back to its built-in users, which
 * is also the way out if a user was added that stops something working.
 *
 * ── No passwords ──
 * There is no passwd command and no password field that means anything.
 * Nothing on this machine authenticates with one: SSH is public-key only
 * and password authentication is not compiled into the server, and there
 * is no login program on the console. A password field that is checked
 * by nothing is worse than none, because it looks like a lock.
 *
 * su from root needs no password and su from anyone else is refused by
 * the kernel, which is the real enforcement either way.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define EXTRA_USERS "/data/users"
#define EXTRA_GROUP "/data/groups"

static bool name_is_sane(const char *s)
{
    if (!*s || *s == '-')
        return false;
    for (const char *p = s; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
            return false;
    }
    return strlen(s) < 32;
}

static int list_users(void)
{
    long fd = lp_open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "useradd: cannot read /etc/passwd\n");
        return 1;
    }
    printf("  %-16s %6s %6s  %-24s %s\n", "name", "uid", "gid", "home", "shell");
    char line[256];
    while (readline((int)fd, line, sizeof line) >= 0) {
        if (line[0] == '#' || !line[0])
            continue;
        char *f[8];
        int n = 0;
        char *p = line;
        while (n < 8) {
            f[n++] = p;
            char *c = strchr(p, ':');
            if (!c) break;
            *c = '\0';
            p = c + 1;
        }
        if (n < 7)
            continue;
        printf("  %-16s %6s %6s  %-24s %s\n", f[0], f[2], f[3], f[5], f[6]);
    }
    lp_close((int)fd);
    printf("\n  Users added here are kept in %s and merged back in\n",
           EXTRA_USERS);
    printf("  at every boot - /etc itself is in RAM.\n");
    return 0;
}

/* The first free uid at or above 1000, looking at what already exists. */
static uid_t next_uid(void)
{
    uid_t want = 1000;
    for (int guard = 0; guard < 5000; guard++) {
        lp_user_t u;
        if (!lp_user_by_uid(want, &u))
            return want;
        want++;
    }
    return 0;
}

static int append_line(const char *path, const char *line)
{
    long fd = lp_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "useradd: cannot write %s (%ld).\n"
                "useradd:   Is the data partition mounted? Without it there\n"
                "useradd:   is nowhere for a user to be kept.\n", path, -fd);
        return 1;
    }
    dprintf((int)fd, "%s\n", line);
    lp_close((int)fd);
    lp_sync();
    return 0;
}

/* Copy `path` back without the lines for this user. */
static int remove_user_from(const char *path, const char *name)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;                       /* nothing there to remove */

    static char kept[8192];
    size_t used = 0;
    char line[256];
    bool removed = false;
    size_t nlen = strlen(name);

    while (readline((int)fd, line, sizeof line) >= 0) {
        if (strncmp(line, name, nlen) == 0 && line[nlen] == ':') {
            removed = true;
            continue;
        }
        size_t l = strlen(line);
        if (used + l + 2 < sizeof kept) {
            memcpy(kept + used, line, l);
            used += l;
            kept[used++] = '\n';
        }
    }
    lp_close((int)fd);

    if (!removed)
        return 0;

    long out = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        return 1;
    lp_write((int)out, kept, used);
    lp_close((int)out);
    lp_sync();
    return 0;
}

static void usage(const char *base)
{
    printf("%s - make a user\n\n", base);
    printf("  useradd [-u uid] [-g gid] [-d home] [-s shell] <name>\n");
    printf("  useradd -l                what users there are\n");
    printf("  userdel <name>\n\n");
    printf("The user is written to %s, which /etc/rc merges into\n", EXTRA_USERS);
    printf("/etc/passwd at every boot - /etc is in RAM and is rebuilt from\n");
    printf("the kernel image each time, so a line appended there would be\n");
    printf("gone by morning.\n\n");
    printf("There are no passwords. Nothing here checks one: SSH is\n");
    printf("public-key only and the server has password authentication\n");
    printf("compiled out. A password field checked by nothing looks like a\n");
    printf("lock and is not one, so there is not one.\n\n");
    printf("  su <name> [-c cmd]    run something as them\n");
    printf("  chown <name> <file>   give them a file\n");
    printf("  id <name>             what they are\n");
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    bool deleting = (strcmp(base, "userdel") == 0);

    const char *name = NULL, *home = NULL, *shell = NULL;
    int want_uid = -1, want_gid = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)      want_uid = atoi(argv[++i]);
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) want_gid = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) home  = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) shell = argv[++i];
        else if (strcmp(argv[i], "-l") == 0)                 return list_users();
        else if (strcmp(argv[i], "-h") == 0)               { usage(base); return 0; }
        else name = argv[i];
    }

    if (!name) {
        usage(base);
        return 2;
    }
    if (!name_is_sane(name)) {
        dprintf(STDERR_FILENO,
                "%s: \"%s\" is not a usable name. Letters, digits, - and _,\n"
                "%s: under 32 characters, not starting with a dash.\n",
                base, name, base);
        return 1;
    }

    if (deleting) {
        lp_user_t u;
        if (!lp_user_by_name(name, &u)) {
            dprintf(STDERR_FILENO, "userdel: there is no user \"%s\"\n", name);
            return 1;
        }
        if (u.uid == 0) {
            dprintf(STDERR_FILENO,
                    "userdel: refusing to delete root.\n");
            return 1;
        }
        int rc = remove_user_from(EXTRA_USERS, name);
        rc |= remove_user_from(EXTRA_GROUP, name);
        /* /etc is in RAM, so taking the line out there too makes the
         * change visible now instead of only after a reboot. */
        remove_user_from("/etc/passwd", name);
        remove_user_from("/etc/group", name);
        if (rc == 0)
            printf("userdel: %s is gone. Their files are not - they still\n"
                   "         belong to uid %d.\n", name, (int)u.uid);
        return rc;
    }

    lp_user_t existing;
    if (lp_user_by_name(name, &existing)) {
        dprintf(STDERR_FILENO,
                "useradd: %s is already a user (uid %d).\n",
                name, (int)existing.uid);
        return 1;
    }

    uid_t uid = (want_uid >= 0) ? (uid_t)want_uid : next_uid();
    if (uid == 0 && want_uid != 0) {
        dprintf(STDERR_FILENO, "useradd: no free user id\n");
        return 1;
    }
    if (uid == 0) {
        dprintf(STDERR_FILENO,
                "useradd: uid 0 is root. A second user with it is not a\n"
                "useradd:   second user, it is root with another name.\n");
        return 1;
    }
    gid_t gid = (want_gid >= 0) ? (gid_t)want_gid : (gid_t)uid;

    char homebuf[96];
    if (!home) {
        snprintf(homebuf, sizeof homebuf, "/data/home/%s", name);
        home = homebuf;
    }
    if (!shell)
        shell = "/bin/sh";

    char line[256];
    snprintf(line, sizeof line, "%s:x:%d:%d:%s:%s:%s",
             name, (int)uid, (int)gid, name, home, shell);
    if (append_line(EXTRA_USERS, line) != 0)
        return 1;

    char gline[128];
    snprintf(gline, sizeof gline, "%s:x:%d:", name, (int)gid);
    append_line(EXTRA_GROUP, gline);

    /* Make it true now as well as after the next boot. */
    append_line("/etc/passwd", line);
    append_line("/etc/group", gline);

    /* Somewhere to put their files, owned by them. Without this the
     * user exists and has nowhere to write, which shows up later as a
     * confusing permission error rather than as this. */
    lp_mkdir("/data/home", 0755);
    if (lp_mkdir(home, 0755) == 0 || lp_is_dir(home))
        lp_chown(home, uid, gid);

    printf("useradd: %s  uid %d  gid %d  home %s\n",
           name, (int)uid, (int)gid, home);
    printf("useradd:   no password - nothing on this machine checks one.\n");
    printf("useradd:   'su %s -c <command>' runs something as them.\n", name);
    return 0;
}
