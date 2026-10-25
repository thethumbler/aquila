#ifndef _STRING_H
#define _STRING_H

#include <core/system.h>

/* The kernel's own string and memory functions, as the C library's. */

/* Formats a string into a buffer (core/snprintf.c). */
int snprintf(char *s, size_t n, const char *fmt, ...);

/* The memory functions (core/string.c): functions, not inline, because
 * the compiler may call them itself. */
void *memmove(void *dest, const void *src, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

/** Compares two strings, as the C library's `strcmp()`. A NULL string is
 * the caller's bug: it faults, rather than comparing "equal". */
static inline int strcmp(const char *s1, const char *s2)
{
    while (*s1 != '\0' && *s2 != '\0' && *s1 == *s2) {s1++; s2++;}

    return (int)(*s1 - *s2);
}

/** Compares at most `n` characters of two strings. */
static inline int strncmp(const char *s1, const char *s2, size_t n)
{
    for (; n != 0; --n, ++s1, ++s2) {
        if (*s1 != *s2)
            return (int) *(const unsigned char *) s1 - (int) *(const unsigned char *) s2;

        if (*s1 == '\0')
            break;
    }

    return 0;
}

/** Returns the length of a string. */
static inline int strlen(const char *s)
{
    const char *_s = s;
    while (*_s) _s++;
    return _s - s;
}

/** Returns the length of a string, or `max` if there is no NUL in its
 * first `max` bytes. */
static inline size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

/** Returns a copy of a string, from `kmalloc()`, or NULL if there's no
 * memory for it. */
static inline char *strdup(const char *s)
{
    int len = strlen(s);
    char *ret = kmalloc(len + 1, &M_BUFFER, 0);

    if (!ret)
        return NULL;

    (void) memcpy(ret, s, len + 1);
    return ret;
}

/** Copies a string, with its NUL, and returns `dst`. */
static inline char *strcpy(char *dst, const char *src)
{
    char *retval = dst;
    
    while (*src != '\0')
        *dst++ = *src++;

    *dst = *src;    /* the NUL */

    return retval;
}

/** Copies a string, as `strcpy()`, but returns the end of `dst` (its
 * NUL). */
static inline char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src++))
        ++dst;

    return dst;
}

/**
 * Splits a string at each `c`, leading ones skipped. Returns an array of
 * the pieces, ended by NULL, which `free_tokens()` frees, or NULL if `s`
 * is NULL or empty, or if there's no memory. The pieces all live in one
 * copy of `s`.
 */
static inline char **tokenize(const char *s, char c)
{
    if (s == NULL || *s == '\0')
        return NULL;

    while (*s == c)
        ++s;

    char *tokens = strdup(s);

    if (tokens == NULL)
        return NULL;

    int len = strlen(s);

    if (!len) {
        /* nothing but separators: no pieces */
        kfree(tokens);

        char **ret = kmalloc(sizeof(char *), &M_BUFFER, 0);

        if (ret)
            *ret = NULL;

        return ret;
    }

    int i, count = 0;
    for (i = 0; i < len; ++i) {
        if (tokens[i] == c) {
            tokens[i] = 0;
            ++count;
        }
    }

    if (s[len-1] != c)
        ++count;
    
    char **ret = kmalloc(sizeof(char *) * (count + 1), &M_BUFFER, 0);

    if (!ret) {
        kfree(tokens);
        return NULL;
    }

    int j = 0;
    ret[j++] = tokens;

    for (i = 0; i < strlen(s) - 1; ++i)
        if (tokens[i] == 0)
            ret[j++] = &tokens[i+1];

    ret[j] = NULL;

    return ret;
}

/** Frees what `tokenize()` returned. */
static inline void free_tokens(char **ptr)
{
    if (!ptr) return;
    if (*ptr)
        kfree(*ptr);
    kfree(ptr);
}

#endif /* !_STRING_H */
