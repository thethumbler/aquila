#ifndef _STDDEF_H
#define _STDDEF_H

/* The standard types and macros, for compilers that have no stddef.h of
 * their own (the kernel has no C library). */

typedef __SIZE_TYPE__ size_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __PTRDIFF_TYPE__ intptr_t;
typedef __SIZE_TYPE__ uintptr_t;

#ifndef __int8_t_defined
#define __int8_t_defined
typedef signed char int8_t;
typedef signed short int int16_t;
typedef signed int int32_t;
typedef signed long long int int64_t;
typedef unsigned char uint8_t;
typedef unsigned short int uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long int uint64_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef offsetof
#define offsetof(type, field) ((size_t)&((type *)0)->field)
#endif

void *alloca(size_t size);

#endif
