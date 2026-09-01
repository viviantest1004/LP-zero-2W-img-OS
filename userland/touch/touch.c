/* touch - create an empty file.
 *
 *   touch <file>...
 *
 * If the file already exists, do nothing - the contents are not cleared.
 * We do not touch the timestamp. There is no build tool on this system,
 * so nothing decides anything from a file's mtime.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: touch <file>...\n");
        return 2;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (lp_exists(argv[i]))
            continue;
        /* No O_EXCL. If someone else created it between the check and here,
         * the file exists, which is what we wanted anyway. */
        long fd = lp_open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "touch: %s: cannot create (%ld)\n",
                    argv[i], -fd);
            rc = 1;
            continue;
        }
        lp_close((int)fd);
    }
    return rc;
}
