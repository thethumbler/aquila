/** \file
 * What the code the compiler generates calls, besides the memory
 * functions (core/string.c): the stack protector's canary and its
 * failure (`-fstack-protector`), and `abort()`.
 */

#include <core/system.h>
#include <core/panic.h>

uintptr_t __stack_chk_guard = 0xDEADBEEF;

/**
 * Panics: a function's stack canary was overwritten, so its stack was
 * smashed (a buffer overflow).
 */
void __stack_chk_fail(void)
{
    panic("stack smashing detected");
}

/**
 * Panics: compiler support code found what it can't handle. tcc's
 * `va_arg()` on x86_64 (`__va_arg()`, in its libtcc1) calls it for a type
 * it can't take.
 */
void abort(void)
{
    panic("abort");
}
