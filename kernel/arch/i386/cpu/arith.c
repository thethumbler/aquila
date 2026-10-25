/** \file
 * i386: 64-bit division and shifts, for the code the compiler generates.
 *
 * A 32-bit CPU has no instruction that divides 64-bit numbers, so GCC
 * calls these helpers (`__divdi3()` and the others) for `/` and `%` on
 * 64-bit integers; tcc calls them too, and others for 64-bit shifts by
 * a variable count (`__ashldi3()`, ...). User programs get them from
 * libgcc or tcc's libtcc1; the kernel is linked with neither of these,
 * so it has its own. The division comes from Pintos
 * (https://www.ida.liu.se/~TDIU25/pintos/src/lib/arithmetic.c).
 */

#include <stdint.h>

/** Divides the 64-bit `n` by the 32-bit `d` with the x86 DIVL
 * instruction, and returns the 32-bit quotient. It traps with a divide
 * error (`#DE`) if the quotient doesn't fit in 32 bits. */
static inline uint32_t divl(uint64_t n, uint32_t d)
{
    uint32_t n1 = n >> 32;
    uint32_t n0 = n;
    uint32_t q, r;

    asm ("divl %4"
             : "=d" (r), "=a" (q)
             : "0" (n1), "1" (n0), "rm" (d));

    return q;
}

/** Returns the number of leading zero bits in `x`, which must not be
 * 0. */
static int nlz(uint32_t x) 
{
    int n = 0;
    if (x <= 0x0000FFFF) {
        n += 16;
        x <<= 16; 
    }

    if (x <= 0x00FFFFFF) {
        n += 8;
        x <<= 8; 
    }

    if (x <= 0x0FFFFFFF) {
        n += 4;
        x <<= 4;
    }

    if (x <= 0x3FFFFFFF) {
        n += 2;
        x <<= 2; 
    }

    if (x <= 0x7FFFFFFF)
        n++;

    return n;
}

/** Divides the unsigned 64-bit `n` by the unsigned 64-bit `d`, and
 * returns the quotient. */
static uint64_t udiv64(uint64_t n, uint64_t d)
{
    if ((d >> 32) == 0) {
        /* Proof of correctness:
         * Let n, d, b, n1, and n0 be defined as in this function.
         * Let [x] be the "floor" of x.  Let T = b[n1/d].  Assume d
         * nonzero.  Then:
         *   [n/d] = [n/d] - T + T
         *         = [n/d - T] + T                      by (1) below
         *         = [(b*n1 + n0)/d - T] + T            by definition of n
         *         = [(b*n1 + n0)/d - dT/d] + T
         *         = [(b(n1 - d[n1/d]) + n0)/d] + T
         *         = [(b[n1 % d] + n0)/d] + T,          by definition of %
         * which is the expression calculated below.
         *
         * (1) Note that for any real x, integer i: [x] + i = [x + i].
         *
         * To prevent divl() from trapping, [(b[n1 % d] + n0)/d] must
         * be less than b.    Assume that [n1 % d] and n0 take their
         * respective maximum values of d - 1 and b - 1:
         *   [(b(d - 1) + (b - 1))/d] < b
         *   <=> [(bd - 1)/d] < b
         *   <=> [b - 1/d] < b
         * which is a tautology.
         *
         * Therefore, this code is correct and will not trap.
         */

        uint64_t b = 1ULL << 32;
        uint32_t n1 = n >> 32;
        uint32_t n0 = n; 
        uint32_t d0 = d;

        return divl(b * (n1 % d0) + n0, d0) + b * (n1 / d0); 
    } else {
        /* Based on the algorithm and proof available from
         * http://www.hackersdelight.org/revisions.pdf.
         */
        if (n < d) {
            return 0;
        } else {
            uint32_t d1 = d >> 32;
            int s = nlz(d1);
            uint64_t q = divl(n >> 1, (d << s) >> 32) >> (31 - s);
            return n - (q - 1) * d < d ? q - 1 : q; 
        }
    }
}

/** Divides the unsigned 64-bit `n` by the unsigned 64-bit `d`, and
 * returns the remainder. */
static uint64_t umod64(uint64_t n, uint64_t d)
{
    return n - d * udiv64(n, d);
}

/** Divides the signed 64-bit `n` by the signed 64-bit `d`, and returns
 * the quotient. */
static int64_t sdiv64(int64_t n, int64_t d)
{
    uint64_t n_abs = n >= 0 ? (uint64_t) n : -(uint64_t) n;
    uint64_t d_abs = d >= 0 ? (uint64_t) d : -(uint64_t) d;
    uint64_t q_abs = udiv64 (n_abs, d_abs);
    return (n < 0) == (d < 0) ? (int64_t) q_abs : -(int64_t) q_abs;
}

/** Divides the signed 64-bit `n` by the signed 64-bit `d`, and returns
 * the remainder. */
static int64_t smod64(int64_t n, int64_t d)
{
    return n - d * sdiv64(n, d);
}

/* The routines that GCC calls. */

long long __divdi3(long long n, long long d);
long long __moddi3(long long n, long long d);
unsigned long long __udivdi3(unsigned long long n, unsigned long long d);
unsigned long long __umoddi3(unsigned long long n, unsigned long long d);
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long *rem);
long long __ashldi3(long long x, int n);
long long __ashrdi3(long long x, int n);
unsigned long long __lshrdi3(unsigned long long x, int n);

/**
 * Divides two signed 64-bit integers.
 *
 * @param n  the dividend
 * @param d  the divisor
 * @return the quotient
 */
long long __divdi3(long long n, long long d) 
{
    return sdiv64(n, d);
}

/**
 * Computes the remainder of two signed 64-bit integers.
 *
 * @param n  the dividend
 * @param d  the divisor
 * @return the remainder
 */
long long __moddi3(long long n, long long d) 
{
    return smod64 (n, d);
}

/**
 * Divides two unsigned 64-bit integers.
 *
 * @param n  the dividend
 * @param d  the divisor
 * @return the quotient
 */
unsigned long long __udivdi3(unsigned long long n, unsigned long long d) 
{
    return udiv64 (n, d);
}

/**
 * Computes the remainder of two unsigned 64-bit integers.
 *
 * @param n  the dividend
 * @param d  the divisor
 * @return the remainder
 */
unsigned long long __umoddi3(unsigned long long n, unsigned long long d) 
{
    return umod64 (n, d);
}

/**
 * Divides two unsigned 64-bit integers, giving both the quotient and the
 * remainder.
 *
 * @param n    the dividend
 * @param d    the divisor
 * @param rem  if not NULL, set to the remainder
 * @return the quotient
 */
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long *rem)
{
    uint64_t q = udiv64(n, d);

    if (rem)
        *rem = n - d * q;

    return q;
}

/* A 64-bit number's two halves. The shifts below work on them: a 64-bit
 * shift would be a call to themselves. */
union halves {
    uint64_t v;
    struct {
        uint32_t lo;
        uint32_t hi;
    } w;
};

/**
 * Shifts a 64-bit integer left.
 *
 * @param x  the value
 * @param n  the count, 0 to 63
 * @return `x << n`
 */
long long __ashldi3(long long x, int n)
{
    union halves h = {.v = (uint64_t) x};

    if (n >= 32) {
        h.w.hi = h.w.lo << (n - 32);
        h.w.lo = 0;
    } else if (n) {
        h.w.hi = h.w.hi << n | h.w.lo >> (32 - n);
        h.w.lo <<= n;
    }

    return (long long) h.v;
}

/**
 * Shifts an unsigned 64-bit integer right, zeros coming in.
 *
 * @param x  the value
 * @param n  the count, 0 to 63
 * @return `x >> n`
 */
unsigned long long __lshrdi3(unsigned long long x, int n)
{
    union halves h = {.v = x};

    if (n >= 32) {
        h.w.lo = h.w.hi >> (n - 32);
        h.w.hi = 0;
    } else if (n) {
        h.w.lo = h.w.lo >> n | h.w.hi << (32 - n);
        h.w.hi >>= n;
    }

    return h.v;
}

/**
 * Shifts a signed 64-bit integer right, copies of its sign coming in.
 *
 * @param x  the value
 * @param n  the count, 0 to 63
 * @return `x >> n`
 */
long long __ashrdi3(long long x, int n)
{
    union halves h = {.v = (uint64_t) x};
    int32_t hi = (int32_t) h.w.hi;

    if (n >= 32) {
        h.w.lo = (uint32_t) (hi >> (n - 32));
        h.w.hi = (uint32_t) (hi >> 31);
    } else if (n) {
        h.w.lo = h.w.lo >> n | h.w.hi << (32 - n);
        h.w.hi = (uint32_t) (hi >> n);
    }

    return (long long) h.v;
}
