/* stdio.c - 최소 printf 계열.
 * 부동소수점은 지원하지 않는다. 셸과 init 에는 필요 없고, 넣으면
 * 코드가 몇 배로 커진다. */
#include "stdio.h"
#include "string.h"
#include "unistd.h"

#include <stdarg.h>

/* 출력 대상을 추상화한다. 파일 디스크립터로 흘리거나(sink_fd)
 * 버퍼에 담거나(sink_buf) 같은 포맷 코드를 공유하기 위함. */
typedef struct {
    int     fd;         /* fd >= 0 이면 파일로, 아니면 버퍼로 */
    char   *buf;
    size_t  cap;        /* buf 크기 (NUL 자리 포함) */
    size_t  len;        /* 지금까지 만들어진 길이 (잘려도 계속 센다) */
    char    chunk[256]; /* fd 모드용 쓰기 버퍼 */
    size_t  chunk_len;
} sink_t;

static void sink_flush(sink_t *s)
{
    if (s->fd >= 0 && s->chunk_len) {
        lp_write(s->fd, s->chunk, s->chunk_len);
        s->chunk_len = 0;
    }
}

static void sink_put(sink_t *s, char c)
{
    s->len++;

    if (s->fd >= 0) {
        s->chunk[s->chunk_len++] = c;
        if (s->chunk_len == sizeof(s->chunk))
            sink_flush(s);
        return;
    }

    /* 버퍼 모드: 넘치면 버리되 len 은 계속 센다 (snprintf 규약) */
    if (s->buf && s->len <= s->cap - 1)
        s->buf[s->len - 1] = c;
}

static void emit_uint(sink_t *s, u64 v, unsigned base, bool upper,
                      unsigned width, bool zero_pad)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;

    char tmp[24];
    unsigned n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v && n < sizeof(tmp)) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }
    while (n < width && n < sizeof(tmp))
        tmp[n++] = zero_pad ? '0' : ' ';

    while (n--)
        sink_put(s, tmp[n]);
}

static void emit_int(sink_t *s, s64 v, unsigned width, bool zero_pad)
{
    if (v < 0) {
        sink_put(s, '-');
        /* -(-2^63) 은 넘친다. 부호를 먼저 떼어내 피한다. */
        u64 mag = (u64)(-(v + 1)) + 1;
        emit_uint(s, mag, 10, false, width ? width - 1 : 0, zero_pad);
    } else {
        emit_uint(s, (u64)v, 10, false, width, zero_pad);
    }
}

static void format(sink_t *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') { sink_put(s, *fmt++); continue; }
        fmt++;

        bool zero_pad = false;
        if (*fmt == '0') { zero_pad = true; fmt++; }

        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (unsigned)(*fmt++ - '0');

        int longness = 0;
        while (*fmt == 'l' || *fmt == 'z') { longness++; fmt++; }

        switch (*fmt) {
        case 'c':
            sink_put(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            size_t plen = strlen(p);
            while (plen < width) { sink_put(s, ' '); width--; }
            while (*p) sink_put(s, *p++);
            break;
        }
        case 'd': case 'i':
            emit_int(s, longness ? va_arg(ap, s64) : (s64)va_arg(ap, s32),
                     width, zero_pad);
            break;
        case 'u':
            emit_uint(s, longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                      10, false, width, zero_pad);
            break;
        case 'x':
            emit_uint(s, longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                      16, false, width, zero_pad);
            break;
        case 'X':
            emit_uint(s, longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                      16, true, width, zero_pad);
            break;
        case 'p':
            sink_put(s, '0'); sink_put(s, 'x');
            emit_uint(s, (u64)(uintptr_t)va_arg(ap, void *), 16, false, 16, true);
            break;
        case '%':
            sink_put(s, '%');
            break;
        case '\0':
            return;
        default:
            sink_put(s, '%');
            sink_put(s, *fmt);
            break;
        }
        fmt++;
    }
}

static int run_fd(int fd, const char *fmt, va_list ap)
{
    sink_t s = { .fd = fd, .buf = NULL, .cap = 0, .len = 0, .chunk_len = 0 };
    format(&s, fmt, ap);
    sink_flush(&s);
    return (int)s.len;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = run_fd(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = run_fd(fd, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    sink_t s = { .fd = -1, .buf = buf, .cap = size, .len = 0, .chunk_len = 0 };

    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);

    if (buf && size) {
        size_t end = (s.len < size - 1) ? s.len : size - 1;
        buf[end] = '\0';
    }
    return (int)s.len;   /* 잘렸어도 필요했던 전체 길이를 돌려준다 */
}

int fputs(const char *s, int fd)
{
    size_t n = strlen(s);
    return (int)lp_write(fd, s, n);
}

int puts(const char *s)
{
    fputs(s, STDOUT_FILENO);
    lp_write(STDOUT_FILENO, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    lp_write(STDOUT_FILENO, &ch, 1);
    return c;
}

long readline(int fd, char *buf, size_t size)
{
    size_t n = 0;

    for (;;) {
        char c;
        long r = lp_read(fd, &c, 1);

        if (r == 0)                       /* EOF */
            return (n == 0) ? -1 : (long)n;
        if (r < 0)
            return r;

        if (c == '\n')
            break;

        /* 백스페이스: 커널 라인 편집이 없을 때를 위한 최소 처리 */
        if (c == 0x7F || c == '\b') {
            if (n) n--;
            continue;
        }

        if (n < size - 1)
            buf[n++] = c;
        /* 넘치면 버린다. 잘림은 호출자가 길이로 알 수 있다. */
    }

    buf[n] = '\0';
    return (long)n;
}
