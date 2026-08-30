/* malloc.c - brk 기반 최소 할당기.
 *
 * 우리 시스템에는 스레드가 없고(셸과 init 만 돈다) 할당 패턴도 단순해서
 * 프리 리스트 하나로 충분하다. 인접 블록 병합까지만 하고 그 이상은
 * 하지 않는다 - 복잡도 대비 이득이 없다. */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

typedef struct block {
    size_t        size;     /* 헤더를 제외한 사용 가능 바이트 */
    struct block *next;     /* 주소 순으로 정렬된 전체 목록 */
    int           free;
} block_t;

#define HDR_SIZE   ((size_t)sizeof(block_t))
#define ALIGN_UP(n) (((n) + 15UL) & ~15UL)   /* 16바이트 정렬 */
#define GROW_MIN    (64UL * 1024UL)          /* brk 를 늘릴 때 최소 단위 */

static block_t *heap_head = NULL;
static char    *brk_cur   = NULL;

/* 현재 break 를 얻거나 늘린다. 실패하면 NULL. */
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
        return NULL;            /* 커널이 거부 */

    void *old = brk_cur;
    brk_cur = (char *)got;
    return old;
}

void *malloc(size_t n)
{
    if (n == 0)
        return NULL;
    n = ALIGN_UP(n);

    /* 1) 프리 리스트에서 first-fit */
    block_t *prev = NULL;
    for (block_t *b = heap_head; b; prev = b, b = b->next) {
        if (!b->free || b->size < n)
            continue;

        /* 남는 공간이 헤더 + 최소 블록보다 크면 쪼갠다 */
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

    /* 2) 힙을 늘린다 */
    void *mem = heap_grow(n + HDR_SIZE);
    if (!mem)
        return NULL;

    block_t *b = mem;
    b->size = n;
    b->free = 0;
    b->next = NULL;

    if (prev) prev->next = b;
    else      heap_head  = b;

    /* heap_grow 가 요청보다 많이 받아왔으면 나머지를 free 블록으로 */
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

    /* 뒤쪽으로 이어진 free 블록들을 하나로 합친다.
     * 목록이 주소 순이므로 next 만 봐도 된다. */
    while (b->next && b->next->free &&
           (char *)b->next == (char *)b + HDR_SIZE + b->size) {
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
    }
}

void *calloc(size_t count, size_t size)
{
    /* 곱셈 넘침 검사 - 넘치면 작은 버퍼를 잡고 크게 쓰는 취약점이 된다 */
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
