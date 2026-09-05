/* wget - download a file.
 *
 *   wget <url> [file]
 *   wget -O <file> <url>
 *
 * http:// is fetched here. https:// is handed to python3 on /data,
 * which has a real TLS stack with OpenSSL inside it - there is none in
 * this userland, and writing one would be the worst possible place to
 * be nearly right. So https costs a few seconds of interpreter startup
 * and needs an image that carries Python; see net_https_get in
 * libc/src/net.c.
 *
 * With no file name given, the last part of the URL path is used.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: wget <url> [file]\n");
        printf("       wget -O <file> <url>\n");
        printf("  https:// works too, by way of python3 on /data -\n");
        printf("  a few seconds slower to start, and it checks the\n");
        printf("  certificate against /data/ssl/cert.pem\n");
        return argc < 2 ? 2 : 0;
    }

    const char *url  = NULL;
    const char *dest = NULL;

    /* -O is accepted because it is what everybody types.
     *
     * This only ever took its arguments positionally, and `wget -O out
     * url` therefore treated "-O" as the URL and printed "-O: this is
     * not an http:// or https:// URL" - which reads as the URL being
     * rejected rather than the option not existing. Every other wget on
     * earth spells it this way; refusing it taught nobody anything. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0) {
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO, "wget: -O needs a file name after it\n");
                return 2;
            }
            dest = argv[++i];
        } else if (!url) {
            url = argv[i];
        } else if (!dest) {
            dest = argv[i];
        }
    }

    if (!url) {
        dprintf(STDERR_FILENO, "wget: no URL given\n");
        return 2;
    }

    char name[256];
    if (!dest) {
        /* The last path component, or index.html when there is none. */
        const char *slash = strrchr(url, '/');
        if (slash && slash[1])
            strlcpy(name, slash + 1, sizeof(name));
        else
            strlcpy(name, "index.html", sizeof(name));
        dest = name;
    }

    printf("wget: %s -> %s\n", url, dest);

    long n = net_http_get(url, dest);
    if (n < 0)
        return 1;

    if (n >= 1048576)
        printf("wget: %lld MB\n", (long long)(n / 1048576));
    else if (n >= 1024)
        printf("wget: %lld KB\n", (long long)(n / 1024));
    else
        printf("wget: %lld bytes\n", (long long)n);
    return 0;
}
