/* stdio.h - minimal I/O with no FILE*. File descriptors are used directly. */
#ifndef _LP_STDIO_H
#define _LP_STDIO_H

#include "types.h"

int  printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int  dprintf(int fd, const char *fmt, ...)
                                   __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t size, const char *fmt, ...)
                                   __attribute__((format(printf, 3, 4)));

int  puts(const char *s);          /* adds a newline */
int  fputs(const char *s, int fd); /* no newline */
int  putchar(int c);

/* Read one line. The newline is dropped and the result NUL terminated.
 * Returns the length read, or -1 at EOF. */
long readline(int fd, char *buf, size_t size);

#endif /* _LP_STDIO_H */
