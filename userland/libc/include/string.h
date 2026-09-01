/* string.h - 문자열/메모리. 자체 구현. */
#ifndef _LP_STRING_H
#define _LP_STRING_H

#include "types.h"

void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strdup(const char *s);

/* 잘림을 알려주는 안전한 복사. 목적지는 항상 NUL 로 끝난다.
 * 복사하려 한 전체 길이를 돌려주므로 >= size 면 잘린 것이다. */
size_t strlcpy(char *d, const char *s, size_t size);
/* d 뒤에 s 를 붙인다. 항상 NUL 로 끝낸다.
 * 반환값은 붙이려던 전체 길이 - size 이상이면 잘렸다는 뜻이다. */
size_t strlcat(char *d, const char *s, size_t size);

long   strtol(const char *s, char **end, int base);

#endif /* _LP_STRING_H */
