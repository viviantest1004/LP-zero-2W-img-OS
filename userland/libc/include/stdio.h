/* stdio.h - FILE* 없는 최소 입출력. 파일 디스크립터를 직접 쓴다. */
#ifndef _LP_STDIO_H
#define _LP_STDIO_H

#include "types.h"

int  printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int  dprintf(int fd, const char *fmt, ...)
                                   __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t size, const char *fmt, ...)
                                   __attribute__((format(printf, 3, 4)));

int  puts(const char *s);          /* 개행을 붙인다 */
int  fputs(const char *s, int fd); /* 개행 없음 */
int  putchar(int c);

/* 한 줄 읽기. 개행은 제거하고 NUL 로 끝낸다.
 * 반환: 읽은 길이, EOF 면 -1. */
long readline(int fd, char *buf, size_t size);

#endif /* _LP_STDIO_H */
