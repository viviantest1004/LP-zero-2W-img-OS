/* uname - what this system is.
 *
 *   uname            the kernel name
 *   uname -a         everything
 *   uname -r         the release
 *   uname -m         the machine
 *
 * From the uname syscall, which fills a struct of six fixed-size fields
 * of 65 bytes each: sysname, nodename, release, version, machine and
 * domainname.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define FIELD 65
#define SYSNAME  (0 * FIELD)
#define NODENAME (1 * FIELD)
#define RELEASE  (2 * FIELD)
#define VERSION  (3 * FIELD)
#define MACHINE  (4 * FIELD)

int main(int argc, char **argv)
{
    char uts[FIELD * 6];
    memset(uts, 0, sizeof(uts));

    if (lp_uname(uts) < 0) {
        dprintf(STDERR_FILENO, "uname: failed\n");
        return 1;
    }

    bool all = false, rel = false, mach = false, node = false, ver = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'a': all = true; break;
            case 'r': rel = true; break;
            case 'm': mach = true; break;
            case 'n': node = true; break;
            case 'v': ver = true; break;
            case 'h':
                printf("usage: uname [-a] [-r] [-m] [-n] [-v]\n");
                printf("  -a all   -r release   -m machine\n");
                printf("  -n hostname   -v version\n");
                return 0;
            default:
                dprintf(STDERR_FILENO, "uname: unknown option -%c\n", *o);
                return 2;
            }
        }
    }

    if (all) {
        printf("%s %s %s %s %s\n",
               uts + SYSNAME, uts + NODENAME, uts + RELEASE,
               uts + VERSION, uts + MACHINE);
        return 0;
    }

    if (!rel && !mach && !node && !ver) {
        printf("%s\n", uts + SYSNAME);
        return 0;
    }

    bool first = true;
    if (node) { printf("%s%s", first ? "" : " ", uts + NODENAME); first = false; }
    if (rel)  { printf("%s%s", first ? "" : " ", uts + RELEASE);  first = false; }
    if (ver)  { printf("%s%s", first ? "" : " ", uts + VERSION);  first = false; }
    if (mach) { printf("%s%s", first ? "" : " ", uts + MACHINE);  first = false; }
    printf("\n");
    return 0;
}
