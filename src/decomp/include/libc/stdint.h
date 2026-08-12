#ifndef STDINT_H
#define STDINT_H

/* liboot: host builds must follow the host pointer ABI.  In particular,
 * Windows x64 is LLP64, so `long` remains 32-bit while pointers are 64-bit. */
#if defined(LIBOOT_HOST_BUILD)
#if defined(__INTPTR_TYPE__) && defined(__UINTPTR_TYPE__)
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
#elif defined(_WIN64)
typedef signed __int64 intptr_t;
typedef unsigned __int64 uintptr_t;
#else
typedef signed long intptr_t;
typedef unsigned long uintptr_t;
#endif
#else
typedef signed long intptr_t;
typedef unsigned long uintptr_t;
#endif

#define INT8_MIN    (-0x80)
#define INT16_MIN   (-0x8000)
#define INT32_MIN   (-0x80000000)
#define INT64_MIN   (-0x8000000000000000)

#define INT8_MAX    0x7F
#define INT16_MAX   0x7FFF
#define INT32_MAX   0x7FFFFFFF
#define INT64_MAX   0x7FFFFFFFFFFFFFFF

#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFF
#define UINT64_MAX  0xFFFFFFFFFFFFFFFF

#if defined(LIBOOT_HOST_BUILD) && defined(__INTPTR_MAX__) && defined(__UINTPTR_MAX__)
#define INTPTR_MIN  (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX  __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__
#elif defined(LIBOOT_HOST_BUILD) && defined(_WIN64)
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#else
#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#endif

#if defined(LIBOOT_HOST_BUILD) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(intptr_t) == sizeof(void*), "intptr_t must match the host pointer size");
_Static_assert(sizeof(uintptr_t) == sizeof(void*), "uintptr_t must match the host pointer size");
#endif

#endif
