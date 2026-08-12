#ifndef STDDEF_H
#define STDDEF_H

#define NULL ((void*)0)

#if !defined(_SIZE_T) && !defined(_SIZE_T_)
#define _SIZE_T

#if defined(LIBOOT_HOST_BUILD)
/* liboot: use the compiler's host ABI type instead of the N64's 32-bit type. */
#if defined(__SIZE_TYPE__)
typedef __SIZE_TYPE__ size_t;
#elif defined(_WIN64)
typedef unsigned __int64 size_t;
#elif defined(_WIN32)
typedef unsigned int size_t;
#else
typedef unsigned long size_t;
#endif
#else
#if !defined(_MIPS_SZLONG) || (_MIPS_SZLONG == 32)
typedef unsigned int    size_t;
#endif
#if defined(_MIPS_SZLONG) && (_MIPS_SZLONG == 64)
typedef unsigned long   size_t;
#endif
#endif

#endif

#if defined(LIBOOT_HOST_BUILD) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(size_t) == sizeof(void*), "size_t must match the host pointer size");
#endif

#if __GNUC__ >= 4
#define offsetof(structure, member) __builtin_offsetof (structure, member)
#else
#define offsetof(structure, member) ((size_t)&(((structure*)0)->member))
#endif

#endif
