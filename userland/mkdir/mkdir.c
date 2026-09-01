/* mkdir - create directories.
 *
 *   mkdir <path>...
 *   mkdir -p <path>...    create parents too, and do not fail if it exists
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define EEXIST 17

/* Create parent directories too. Skip ones that already exist. */
static int mkdir_p(const char *path, mode_t mode)
{
    char buf[512];
    if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
        dprintf(STDERR_FILENO, "mkdir: path too long: %s\n", path);
        return 1;
    }

    /* At every '/', create the path up to that point. */
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        long rc = lp_mkdir(buf, mode);
        if (rc < 0 && rc != -EEXIST) {
            dprintf(STDERR_FILENO, "mkdir: %s: failed (%ld)\n", buf, -rc);
            return 1;
        }
        *p = '/';
    }

    long rc = lp_mkdir(buf, mode);
    if (rc < 0 && rc != -EEXIST) {
        dprintf(STDERR_FILENO, "mkdir: %s: failed (%ld)\n", buf, -rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool parents = false;
    int  first = 1;

    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        parents = true;
        first = 2;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] <path>...\n");
        return 2;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (parents) {
            rc |= mkdir_p(argv[i], 0755);
        } else {
            long r = lp_mkdir(argv[i], 0755);
            if (r < 0) {
                dprintf(STDERR_FILENO, "mkdir: %s: failed (%ld)\n", argv[i], -r);
                rc = 1;
            }
        }
    }
    return rc;
}
