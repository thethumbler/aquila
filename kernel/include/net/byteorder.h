#ifndef _NET_BYTEORDER_H
#define _NET_BYTEORDER_H

#include <core/system.h>

/* Converts between the network's byte order (big-endian) and the CPU's.
 * Every CPU that AquilaOS runs on (x86, aarch64) is little-endian, where
 * they swap the bytes; on a big-endian one they would do nothing. */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define htons(x)    ((uint16_t) (x))
#define htonl(x)    ((uint32_t) (x))
#else
#define htons(x)    bswap16((uint16_t) (x))
#define htonl(x)    bswap32((uint32_t) (x))
#endif

/* A value with its bytes in the other order (in plain C, which every
 * compiler takes; GCC and clang make it one instruction). */
static inline uint16_t bswap16(uint16_t x)
{
    return (uint16_t) (x << 8 | x >> 8);
}

static inline uint32_t bswap32(uint32_t x)
{
    return x << 24 | (x & 0xFF00) << 8 | (x >> 8 & 0xFF00) | x >> 24;
}

#define ntohs(x)    htons(x)
#define ntohl(x)    htonl(x)

#endif /* ! _NET_BYTEORDER_H */
