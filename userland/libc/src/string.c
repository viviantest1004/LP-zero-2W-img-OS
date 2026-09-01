/* string.c - 문자열/메모리 구현. */
#include "string.h"
#include "stdlib.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    u8  v = (u8)c;

    while (n && ((uintptr_t)d & 7)) { *d++ = v; n--; }

    u64 w = 0x0101010101010101UL * v;
    while (n >= 8) { *(u64 *)d = w; d += 8; n -= 8; }

    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        while (n >= 8) { *(u64 *)d = *(const u64 *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);

    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = '\0';
    return r;
}

size_t strlcpy(char *d, const char *s, size_t size)
{
    size_t slen = strlen(s);

    if (size) {
        size_t copy = (slen >= size) ? size - 1 : slen;
        memcpy(d, s, copy);
        d[copy] = '\0';
    }
    return slen;    /* >= size 면 잘렸다는 뜻 */
}

size_t strlcat(char *d, const char *s, size_t size)
{
    /* d 안에서 NUL 을 찾는다. size 안에 NUL 이 없으면 d 는 문자열이
     * 아니므로 아무것도 하지 않고 붙이려던 길이만 돌려준다. */
    size_t dlen = 0;
    while (dlen < size && d[dlen] != '\0')
        dlen++;

    size_t slen = strlen(s);
    if (dlen == size)
        return size + slen;

    size_t room = size - dlen;      /* NUL 자리 포함 */
    size_t copy = (slen >= room) ? room - 1 : slen;
    memcpy(d + dlen, s, copy);
    d[dlen + copy] = '\0';
    return dlen + slen;             /* >= size 면 잘렸다 */
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;

    size_t nlen = strlen(needle);
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, nlen) == 0)
            return (char *)hay;
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

long strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    long val = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;

        if (d >= base) break;
        val = val * base + d;
        s++;
    }

    if (end) *end = (char *)s;
    return neg ? -val : val;
}
