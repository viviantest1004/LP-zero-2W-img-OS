/* run - run a program built for an ordinary Linux.
 *
 *   run <program> [arguments...]
 *
 * Every program in this system is statically linked against our own
 * libc, so there is no dynamic loader in the image and a binary built
 * anywhere else fails at execve: the kernel looks for the interpreter
 * named inside it and finds nothing.
 *
 * glibc is on /data, because CPython needs it. This hands a program
 * straight to that loader, which is what /lib/ld-linux-aarch64.so.1
 * would do if it existed:
 *
 *   run ./ripgrep --version
 *
 * /etc/rc makes those /lib symlinks when /data/glibc is present, so
 * most binaries run without this. It is still here for the cases where
 * they do not: a binary that wants libraries somewhere else, or a
 * system where the symlinks were deliberately left out.
 *
 *   run -l /data/lib/mine ./program     look for libraries there too
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LOADER "/data/glibc/ld-linux-aarch64.so.1"
#define LIBDIR "/data/glibc"

int main(int argc, char **argv)
{
    const char *libpath = LIBDIR;
    int first = 1;

    if (argc > 2 && strcmp(argv[1], "-l") == 0) {
        libpath = argv[2];
        first = 3;
    }

    if (argc <= first || strcmp(argv[1], "-h") == 0) {
        printf("usage: run [-l libdir] <program> [arguments...]\n\n");
        printf("Runs a program built for an ordinary Linux, using the\n");
        printf("glibc on /data. Programs built for this system do not\n");
        printf("need it - they have no loader at all.\n\n");
        printf("  loader   %s\n", LOADER);
        printf("  libs     %s\n", LIBDIR);
        return argc <= first ? 2 : 0;
    }

    if (!lp_exists(LOADER)) {
        dprintf(STDERR_FILENO,
                "run: %s is not there.\n"
                "     glibc lives on the data partition and arrives with\n"
                "     CPython. An image without python has no loader, and\n"
                "     programs built elsewhere cannot run on it.\n", LOADER);
        return 1;
    }

    /* ld.so takes the program and its arguments after its own. */
    static char *args[64];
    int n = 0;

    args[n++] = (char *)LOADER;
    args[n++] = (char *)"--library-path";
    args[n++] = (char *)libpath;

    for (int i = first; i < argc && n < 63; i++)
        args[n++] = argv[i];
    args[n] = NULL;

    extern char **environ;
    lp_execve(LOADER, args, environ);

    dprintf(STDERR_FILENO, "run: cannot start %s\n", LOADER);
    return 127;
}
