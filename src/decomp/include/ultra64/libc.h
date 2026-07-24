#ifndef ULTRA64_LIBC_H
#define ULTRA64_LIBC_H

#include "stddef.h"

/* liboot: host glibc declares the BSD functions with size_t, which conflicts
   with the N64 'int' prototypes here; route them to the string.h builtins. */
#define bzero( s, n )       __builtin_memset( ( s ), 0, ( n ))
#define bcmp( a, b, n )     __builtin_memcmp( ( a ), ( b ), ( n ))
#define bcopy( src, dst, n) __builtin_memmove( ( dst ), ( src ), ( n ))

void osSyncPrintf(const char* fmt, ...);

#endif
