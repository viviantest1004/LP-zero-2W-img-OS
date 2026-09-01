/* stdlib.h - memory allocation and the environment. */
#ifndef _LP_STDLIB_H
#define _LP_STDLIB_H

#include "types.h"

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

char *getenv(const char *name);
int   atoi(const char *s);

#endif /* _LP_STDLIB_H */
