/* malloc.c - a minimal brk-based allocator.
 *
 * There are no threads here - only the shell and init run - and the
 * allocation pattern is simple, so one free list is enough. It coalesces
 * adjacent blocks and stops there; more is not worth the complexity. */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

typedef struct block {
    size_t        size;     /* usable bytes, not counting the header */
    struct block *next;     /* the whole list, in address order */
    int           free;
} block_t;

#define HDR_SIZE   ((size_t)sizeof(block_t))
#define ALIGN_UP(n) (((n) + 15UL) & ~15UL)   /* 16-byte alignment */
#define GROW_MIN    (64UL * 1024UL)          /* smallest step when growing brk */

static block_t *heap_head = NULL;
static char    *brk_cur   = NULL;

/* Get or extend the current break. NULL on failure. */
static void *heap_grow(size_t need)
{
    if (brk_cur == NULL) {
        long cur = sys_call1(SYS_brk, 0);
        if (cur <= 0)
            return NULL;
        brk_cur = (char *)cur;
    }

    size_t grow = need < GROW_MIN ? GROW_MIN : ALIGN_UP(need);
    char  *want = brk_cur + grow;

    long got = sys_call1(SYS_brk, (long)want);
    if ((char *)got < want)
        return NULL;            /* the kernel refused */

    void *old = brk_cur;
    brk_cur = (char *)got;
    return old;
}

void *malloc(size_t n)
{
    if (n == 0)
        return NULL;
    n = ALIGN_UP(n);

    /* 1) first fit from the free list */
    block_t *prev = NULL;
    for (block_t *b = heap_head; b; prev = b, b = b->next) {
        if (!b->free || b->size < n)
            continue;

        /* Split it if the leftover exceeds a header plus a minimum block. */
        if (b->size >= n + HDR_SIZE + 16) {
            block_t *split = (block_t *)((char *)b + HDR_SIZE + n);
            split->size = b->size - n - HDR_SIZE;
            split->free = 1;
            split->next = b->next;
            b->size = n;
            b->next = split;
        }
        b->free = 0;
        return (char *)b + HDR_SIZE;
    }

    /* 2) grow the heap */
    void *mem = heap_grow(n + HDR_SIZE);
    if (!mem)
        return NULL;

    block_t *b = mem;
    b->size = n;
    b->free = 0;
    b->next = NULL;

    if (prev) prev->next = b;
    else      heap_head  = b;

    /* If heap_grow took more than we asked, the rest becomes a free block. */
    size_t used = HDR_SIZE + n;
    size_t have = (size_t)(brk_cur - (char *)mem);
    if (have >= used + HDR_SIZE + 16) {
        block_t *rest = (block_t *)((char *)mem + used);
        rest->size = have - used - HDR_SIZE;
        rest->free = 1;
        rest->next = NULL;
        b->next = rest;
    }
    return (char *)b + HDR_SIZE;
}

void free(void *p)
{
    if (!p)
        return;

    block_t *b = (block_t *)((char *)p - HDR_SIZE);
    b->free = 1;

    /* Merge the free blocks that follow into one. The list is in address
     * order, so looking at next is enough. */
    while (b->next && b->next->free &&
           (char *)b->next == (char *)b + HDR_SIZE + b->size) {
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
    }
}

void *calloc(size_t count, size_t size)
{
    /* Check the multiplication for overflow. Without this, an overflow
     * hands back a small buffer that the caller writes to as if it were big. */
    if (count && size > (size_t)-1 / count)
        return NULL;

    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)  return malloc(n);
    if (!n)  { free(p); return NULL; }

    block_t *b = (block_t *)((char *)p - HDR_SIZE);
    if (b->size >= n)
        return p;

    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, b->size);
    free(p);
    return np;
}

char *getenv(const char *name)
{
    extern char **environ;
    if (!environ)
        return NULL;

    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}
