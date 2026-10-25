#ifndef _CORE_SYSTEM_H
#define _CORE_SYSTEM_H

/* What nearly every kernel file needs: small macros, the compiler's
 * attributes, the basic types, printk() and kmalloc(). */

/* A bit's value, the larger or smaller of two values, a value without
 * its sign. */
#define _BV(b) (1 << (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ABS(a) ((a) < 0? -(a) : (a))

/* The size of a structure's member. */
#define MEMBER_SIZE(type, member) (sizeof(((type *)0)->member))

/* Marks what may be unused, without a warning (not with every
 * compiler). */
#if defined(__TINYC__) || defined(__clang__) || defined(__PCC__)
  #undef __unused
  #define __unused
#else
  #ifndef __unused
    #define __unused    __attribute__((unused))
  #endif
#endif


/* A structure with no padding, aligned data, data in a given section. */
#define __packed     __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __section(s) __attribute__((section(s)))

#if defined(__TINYC__) || defined(__PCC__)
#include <stddef.h>
#else
#include_next <stddef.h>
#include_next <stdint.h>
#endif

/* tcc has GCC's __atomic built-ins but not their memory orders, nor the
 * forms that take and give a value (as its <stdatomic.h> defines them,
 * which needs <stdint.h>). */
#ifdef __TINYC__
#define __ATOMIC_RELAXED    0
#define __ATOMIC_CONSUME    1
#define __ATOMIC_ACQUIRE    2
#define __ATOMIC_RELEASE    3
#define __ATOMIC_ACQ_REL    4
#define __ATOMIC_SEQ_CST    5

#define __atomic_load_n(ptr, order) \
    ({ __typeof__(*(ptr)) __v; __atomic_load((ptr), &__v, (order)); __v; })
#define __atomic_store_n(ptr, val, order) \
    ({ __typeof__(*(ptr)) __v = (val); __atomic_store((ptr), &__v, (order)); })
#endif

#include <core/printk.h>
#include <mm/kvmem.h>

#include <config.h>
#include <core/types.h>

#endif /* ! _CORE_SYSTEM_H */
