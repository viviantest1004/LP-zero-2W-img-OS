/* help - list every command on the system.
 *
 *   help            everything, grouped
 *   help <name>     what one command does, and how to run it
 *   help -a         plain list, one name per line (for scripts)
 *
 * This scans the directories in PATH rather than printing a fixed list,
 * so a program dropped into /data/bin shows up here without anyone
 * editing this file. Ones we know about get a description; the rest are
 * listed under "other" so they are at least visible.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

#define MAX_CMDS   256
#define NAME_MAX   64

typedef struct {
    const char *name;
    const char *group;
    const char *what;
    const char *usage;
} entry_t;

/* Groups are printed in this order. */
static const char *GROUPS[] = {
    "shell", "files", "text", "time", "system", "network", "python", NULL
};

static const entry_t KNOWN[] = {
    /* shell builtins - these live in sh, not on disk */
    { "cd",       "shell",   "change directory",            "cd [dir]" },
    { "pwd",      "shell",   "print the current directory", "pwd" },
    { "echo",     "shell",   "print the arguments",         "echo ..." },
    { "env",      "shell",   "list environment variables",  "env" },
    { "exit",     "shell",   "leave the shell",             "exit [code]" },
    { "reboot",   "shell",   "restart the machine",         "reboot" },
    { "poweroff", "shell",   "shut the machine down",       "poweroff" },
    { "help",     "shell",   "this list",                   "help [command]" },

    { "ls",       "files",   "list a directory",            "ls [-l] [-a] [path]" },
    { "cp",       "files",   "copy files",                  "cp [-r] [-n] [-q] <src>... <dst>" },
    { "mv",       "files",   "move or rename",              "mv <src>... <dst>" },
    { "rm",       "files",   "delete files",                "rm [-r] [-f] <path>..." },
    { "mkdir",    "files",   "create directories",          "mkdir [-p] <path>..." },
    { "touch",    "files",   "create an empty file",        "touch <file>..." },
    { "mount",    "files",   "mount a filesystem",          "mount [-t type] <dev> <dir>" },
    { "umount",   "files",   "unmount a filesystem",        "umount <dir>" },
    { "expandfs", "files",   "grow /data to fill the card", "expandfs [disk part]" },

    { "cat",      "text",    "print a file",                "cat [file]..." },
    { "edit",     "text",    "edit a file on screen",       "edit <file>" },

    { "date",     "time",    "show or set the clock",       "date [-u|-e|-s TIME|-z ZONE]" },
    { "ntp",      "time",    "set the clock from the net",  "ntp [-r|-d] [server]" },

    { "top",      "system",  "what is running, and stop it","top [-1] [-n count]" },
    { "kill",     "system",  "stop a process",              "kill [-9] <pid>..." },
    { "sleep",    "system",  "wait",                        "sleep <seconds>" },
    { "watchdog", "system",  "reboot the board if it hangs","watchdog [-t s] [-x]" },
    { "logd",     "system",  "collect logs to /data/log",   "(started by init)" },
    { "sysinfo",  "system",  "memory, CPU, disks, network", "sysinfo" },
    { "zram",     "system",  "compressed swap in RAM",      "zram on|off|status" },
    { "guard",    "system",  "the safety net (memory, heat, power, CPU)",
                                                             "guard [-d]" },
    { "bootcount","system",  "detect a reboot loop",        "bootcount" },
    { "calc",     "system",  "integer calculator",          "calc \"1 + 2 * 3\"" },

    { "dhcp",     "network", "get an address from a router","dhcp <interface>" },
    { "wpa_supplicant", "network", "join a WiFi network",   "wpa_supplicant -B -i wlan0 -c <conf>" },
    { "wpa_cli",  "network", "talk to wpa_supplicant",      "wpa_cli" },
    { "dropbear", "network", "the SSH server",              "(started by init)" },
    { "dropbearkey", "network", "make an SSH host key",     "dropbearkey -t ed25519 -f <file>" },

    { "python",   "python",  "CPython 3.12",                "python [file]" },
    { "python3",  "python",  "the same as python",          "python3 [file]" },
    { "micropython", "python", "MicroPython - small, fast", "micropython [file]" },
    { NULL, NULL, NULL, NULL }
};

static const entry_t *lookup(const char *name)
{
    for (int i = 0; KNOWN[i].name; i++)
        if (strcmp(KNOWN[i].name, name) == 0)
            return &KNOWN[i];
    return NULL;
}

/* Names found on disk, so we can report ones we have no description for. */
static char found[MAX_CMDS][NAME_MAX];
static int  nfound = 0;

static bool already_found(const char *name)
{
    for (int i = 0; i < nfound; i++)
        if (strcmp(found[i], name) == 0)
            return true;
    return false;
}

static void scan_dir(const char *dir)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;                     /* not every PATH entry exists */

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (name[0] == '.' || type == DT_DIR)
                continue;
            if (nfound >= MAX_CMDS || already_found(name))
                continue;
            strlcpy(found[nfound++], name, NAME_MAX);
        }
    }
    lp_close((int)fd);
}

static void scan_path(void)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";

    char dir[256];
    while (*path) {
        size_t i = 0;
        while (*path && *path != ':' && i < sizeof(dir) - 1)
            dir[i++] = *path++;
        dir[i] = '\0';
        if (i)
            scan_dir(dir);
        if (*path == ':')
            path++;
    }
}

/* Is this name a program we actually found, or a shell builtin? */
static bool available(const entry_t *e)
{
    if (strcmp(e->group, "shell") == 0)
        return true;                /* builtins are always there */
    return already_found(e->name);
}

static void print_one(const entry_t *e)
{
    printf("  %-14s %s\n", e->name, e->what);
}

static void print_all(void)
{
    printf("LP-zero OS commands\n");

    for (int g = 0; GROUPS[g]; g++) {
        bool header = false;
        for (int i = 0; KNOWN[i].name; i++) {
            if (strcmp(KNOWN[i].group, GROUPS[g]) != 0)
                continue;
            if (!available(&KNOWN[i]))
                continue;           /* python may not be installed */
            if (!header) {
                printf("\n%s\n", GROUPS[g]);
                header = true;
            }
            print_one(&KNOWN[i]);
        }
    }

    /* Anything on disk we have no description for. A new program shows
     * up here the moment it is copied in, which beats it being invisible. */
    bool other = false;
    for (int i = 0; i < nfound; i++) {
        if (lookup(found[i]))
            continue;
        if (!other) {
            printf("\nother\n");
            other = true;
        }
        printf("  %s\n", found[i]);
    }

    printf("\n"
           "  help <command>   how to use one of them\n"
           "  <command> -h     most of them explain themselves too\n");
    printf("\n"
           "The shell takes < > >> for redirection, | for pipes,\n"
           "and && || ; between commands. It has no variables,\n"
           "no wildcards and no if.\n");
    printf("\n"
           "Files under /data and /root survive a reboot.\n"
           "Everything else is in RAM and does not.\n");
}

int main(int argc, char **argv)
{
    scan_path();

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        /* Plain list, for scripting or just piping into something. */
        for (int i = 0; KNOWN[i].name; i++)
            if (strcmp(KNOWN[i].group, "shell") == 0)
                printf("%s\n", KNOWN[i].name);
        for (int i = 0; i < nfound; i++)
            printf("%s\n", found[i]);
        return 0;
    }

    if (argc > 1) {
        const entry_t *e = lookup(argv[1]);
        if (!e) {
            if (already_found(argv[1])) {
                printf("%s\n", argv[1]);
                printf("  No description here. Try: %s -h\n", argv[1]);
                return 0;
            }
            dprintf(STDERR_FILENO,
                    "help: no command called '%s'\n"
                    "      run 'help' to see what there is\n", argv[1]);
            return 1;
        }
        printf("%s - %s\n\n", e->name, e->what);
        printf("  usage: %s\n", e->usage);
        if (!available(e))
            printf("\n  Not installed on this system.\n");
        return 0;
    }

    print_all();
    return 0;
}
