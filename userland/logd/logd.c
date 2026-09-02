/* logd - collect log messages into a file that survives a reboot.
 *
 *   logd              run in the foreground (init supervises it)
 *   logd -f <path>    where to write (default /data/log/messages)
 *   logd -s <KB>      rotate when the file passes this size (default 512)
 *
 * Without this, everything a program prints goes to the console and is
 * gone the moment it scrolls. If the board wedges at 3am there is no
 * record of what happened - which is exactly when you want one.
 *
 * Two sources are collected:
 *   /dev/kmsg     the kernel's own ring buffer, read as a stream
 *   /dev/log      a unix datagram socket, where our programs can send
 *                 a line without caring where it ends up
 *
 * The file is rotated, not grown forever: at the limit it becomes
 * messages.1 and a fresh one starts. Exactly one old file is kept.
 * An SD card is small and this must never be what fills it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define DEFAULT_PATH  "/data/log/messages"
#define DEFAULT_KB    512
#define SOCK_PATH     "/dev/log"
#define KMSG_PATH     "/dev/kmsg"

#define AF_UNIX       1
#define SOCK_DGRAM_   2

/* struct sockaddr_un { u16 family; char path[108]; } */
typedef struct {
    u16  family;
    char path[108];
} sockaddr_un_t;

static const char *log_path = DEFAULT_PATH;
static long        max_bytes = DEFAULT_KB * 1024;
static long        written   = 0;
static int         out_fd    = -1;

static void open_log(void)
{
    long fd = lp_open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "logd: cannot open %s (%ld)\n", log_path, -fd);
        out_fd = -1;
        return;
    }
    out_fd = (int)fd;

    lp_stat_t st;
    written = (lp_stat(log_path, &st, true) == 0) ? (long)st.size : 0;
}

/* At the limit, move the file aside and start a new one. Keeping one
 * generation is the whole policy - more would need pruning logic that
 * could itself fail and fill the card. */
static void rotate(void)
{
    char old[512];
    if (snprintf(old, sizeof(old), "%s.1", log_path) >= (int)sizeof(old))
        return;

    if (out_fd >= 0)
        lp_close(out_fd);

    lp_unlink(old);
    lp_rename(log_path, old);
    open_log();
}

static void emit(const char *tag, const char *msg, size_t len)
{
    if (out_fd < 0) {
        open_log();
        if (out_fd < 0)
            return;
    }

    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);

    char head[96];
    int  hlen = snprintf(head, sizeof(head),
                         "%d-%02d-%02d %02d:%02d:%02d %s: ",
                         tm.year, tm.mon, tm.day,
                         tm.hour, tm.min, tm.sec, tag);

    lp_write(out_fd, head, (size_t)hlen);
    lp_write(out_fd, msg, len);
    if (len == 0 || msg[len - 1] != '\n')
        lp_write(out_fd, "\n", 1);

    written += hlen + (long)len + 1;
    if (written >= max_bytes)
        rotate();
}

/* /dev/kmsg lines look like "6,123,456789,-;the message".
 * Everything before the ';' is metadata we do not need. */
static const char *strip_kmsg(const char *line, size_t *len)
{
    for (size_t i = 0; i < *len; i++) {
        if (line[i] == ';') {
            *len -= i + 1;
            return line + i + 1;
        }
    }
    return line;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            long kb = atoi(argv[++i]);
            if (kb < 16) kb = 16;
            max_bytes = kb * 1024;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: logd [-f path] [-s KB]\n");
            printf("  -f <path>  where to write (default %s)\n", DEFAULT_PATH);
            printf("  -s <KB>    rotate past this size (default %d)\n", DEFAULT_KB);
            printf("\nRead the log with:  cat %s\n", DEFAULT_PATH);
            return 0;
        } else {
            dprintf(STDERR_FILENO, "logd: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    open_log();
    if (out_fd < 0)
        return 1;

    emit("logd", "started", 7);

    /* The socket other programs send to. Datagrams, so a sender never
     * blocks on us and never needs to connect. */
    lp_unlink(SOCK_PATH);
    long sfd = lp_socket(AF_UNIX, SOCK_DGRAM_, 0);
    if (sfd >= 0) {
        sockaddr_un_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.family = AF_UNIX;
        strlcpy(addr.path, SOCK_PATH, sizeof(addr.path));
        if (lp_bind((int)sfd, &addr, sizeof(addr)) < 0) {
            lp_close((int)sfd);
            sfd = -1;
        }
    }

    long kfd = lp_open(KMSG_PATH, O_RDONLY | O_NONBLOCK, 0);

    /* Poll both sources. Neither is busy, so a short sleep between
     * passes costs nothing and keeps this off the CPU. */
    char buf[2048];
    for (;;) {
        bool did = false;

        if (kfd >= 0) {
            for (int n = 0; n < 32; n++) {
                long r = lp_read((int)kfd, buf, sizeof(buf) - 1);
                if (r <= 0)
                    break;
                size_t len = (size_t)r;
                const char *msg = strip_kmsg(buf, &len);
                emit("kernel", msg, len);
                did = true;
            }
        }

        if (sfd >= 0) {
            for (int n = 0; n < 32; n++) {
                long r = lp_recvfrom((int)sfd, buf, sizeof(buf) - 1, 0, NULL, NULL);
                if (r <= 0)
                    break;
                emit("system", buf, (size_t)r);
                did = true;
            }
        }

        lp_sleep_ms(did ? 50 : 500);
    }
}
