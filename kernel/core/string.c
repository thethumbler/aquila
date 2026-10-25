/** \file
 * The memory functions, as the C library's: copying, setting and
 * comparing bytes. They are functions, not inline as the string ones are
 * (core/string.h), because the compiler may call them itself, to set or
 * copy a structure (C allows it; tcc and clang do).
 */

#include <core/string.h>

/**
 * Copies `n` bytes from `src` to `dest`; the two may overlap.
 *
 * @param dest  where to copy to
 * @param src   where to copy from
 * @param n     how many bytes
 * @return `dest`
 */
void *memmove(void *dest, const void *src, size_t n)
{
    void *_dest = dest;

    if ((uintptr_t) dest < (uintptr_t) src) {
        while (n-- != 0)
            *(char *) dest++ = *(char *) src++;
    } else if ((uintptr_t) dest > (uintptr_t) src) {
        while (n != 0) {
            --n;
            *((char *) dest + n) = *((char *) src + n);
        }
    }

    return _dest;
}

/**
 * Copies `n` bytes from `src` to `dest`, which must not overlap.
 *
 * @param dest  where to copy to
 * @param src   where to copy from
 * @param n     how many bytes
 * @return `dest`
 */
void *memcpy(void *dest, const void *src, size_t n)
{
    void *_dest = dest;
    while (n-- != 0)
        *(char *) dest++ = *(char *) src++;

    return _dest;
}

/**
 * Sets `n` bytes at `s` to `c`.
 *
 * @param s  the bytes
 * @param c  the value, as an unsigned char
 * @param n  how many bytes
 * @return `s`
 */
void *memset(void *s, int c, size_t n)
{
    void *_s = s;

    while (n-- != 0) {
        *(char *) s++ = (char) c;
    }

    return _s;
}

/**
 * Compares `n` bytes.
 *
 * @param s1  the first bytes
 * @param s2  the second bytes
 * @param n   how many bytes
 * @return 0 if they're the same, else the difference of the first bytes
 *         that differ
 */
int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = s1, *b = s2;

    for (; n; --n, ++a, ++b)
        if (*a != *b)
            return *a - *b;

    return 0;
}
