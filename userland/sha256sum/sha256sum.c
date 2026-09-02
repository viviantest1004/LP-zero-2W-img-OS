/* sha256sum - the checksum of a file.
 *
 *   sha256sum <file>...
 *   sha256sum -c <expected> <file>
 *
 * For checking that a download is the file it was meant to be. The
 * output is the same shape as everywhere else - the hash, two spaces,
 * the name - so it can be compared against what a website printed.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: sha256sum <file>...\n");
        printf("       sha256sum -c <expected> <file>\n");
        return argc < 2 ? 2 : 0;
    }

    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 4) {
            dprintf(STDERR_FILENO, "usage: sha256sum -c <expected> <file>\n");
            return 2;
        }
        char hex[72];
        if (!lp_sha256_file(argv[3], hex)) {
            dprintf(STDERR_FILENO, "sha256sum: %s: cannot read\n", argv[3]);
            return 1;
        }
        if (strcmp(hex, argv[2]) == 0) {
            printf("%s: ok\n", argv[3]);
            return 0;
        }
        printf("%s: DOES NOT MATCH\n", argv[3]);
        printf("  expected %s\n", argv[2]);
        printf("  got      %s\n", hex);
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char hex[72];
        if (!lp_sha256_file(argv[i], hex)) {
            dprintf(STDERR_FILENO, "sha256sum: %s: cannot read\n", argv[i]);
            rc = 1;
            continue;
        }
        printf("%s  %s\n", hex, argv[i]);
    }
    return rc;
}
