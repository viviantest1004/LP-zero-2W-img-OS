/* osname.h - what this system calls itself.
 *
 * One place, because the name appears at boot, in `uname`, in `top`'s
 * header, in `sysinfo` and in `help`, and a name that is right in five
 * of those and stale in the sixth is worse than either.
 *
 * The Makefile sets it: the arm64 build is LP-zero, the machine this
 * was written for; the amd64 build is linux-LP, which is the same
 * system on an ordinary desktop and says so rather than claiming to be
 * a Raspberry Pi. */
#ifndef _LP_OSNAME_H
#define _LP_OSNAME_H

#ifndef LP_OS_NAME
#  define LP_OS_NAME "LP-zero"
#endif

#endif
