/* types.h - 자체 libc 기본 타입. 시스템 헤더를 쓰지 않는다. */
#ifndef _LP_TYPES_H
#define _LP_TYPES_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long       u64;   /* LP64: long = 64bit */
typedef signed long         s64;

typedef unsigned long       size_t;
typedef signed long         ssize_t;
typedef signed long         off_t;
typedef signed int          pid_t;
typedef unsigned int        mode_t;
typedef unsigned int        uid_t;
typedef unsigned int        gid_t;
typedef signed long         time_t;
typedef unsigned long       uintptr_t;

#define NULL ((void *)0)

typedef _Bool bool;
#define true  1
#define false 0

_Static_assert(sizeof(long) == 8, "AArch64 LP64 를 가정한다");

#endif /* _LP_TYPES_H */
