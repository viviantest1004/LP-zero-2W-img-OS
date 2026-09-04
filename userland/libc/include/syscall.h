/* syscall.h - pick the system call table for the machine being built.
 *
 * Everything above this line in the source tree is architecture-neutral:
 * the whole userland goes through sys_callN() and the SYS_* names, and
 * those are the only two things that differ between an arm64 board and
 * an amd64 desktop. Splitting the header was the entire port.
 *
 * The numbers really are unrelated between the two - x86-64 carries its
 * own table from before the asm-generic one existed - so there is no
 * clever shared list, just two of them.
 */
#ifndef _LP_SYSCALL_H
#define _LP_SYSCALL_H

#if defined(__aarch64__)
#  include "syscall-arm64.h"
#elif defined(__x86_64__)
#  include "syscall-x86_64.h"
#else
#  error "no system call table for this machine - see syscall-arm64.h"
#endif

#endif
