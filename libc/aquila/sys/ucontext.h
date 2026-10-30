#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A signal handler's third argument: the context the signal interrupted,
 * as the kernel's struct aquila_ucontext (<aquila/ucontext.h>, which
 * abi-check.c compares with this). sigreturn puts back the mask and the
 * registers from it when the handler returns, so a handler may change
 * them. There's no getcontext() or makecontext() (POSIX removed them).
 */

/* the registers, by name */
#if defined(__x86_64__)
typedef struct {
    unsigned long rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
    unsigned long rip, rflags;
} mcontext_t;
#elif defined(__i386__)
typedef struct {
    unsigned long eax, ebx, ecx, edx, esi, edi, ebp, esp;
    unsigned long eip, eflags;
} mcontext_t;
#elif defined(__aarch64__)
typedef struct {
    unsigned long x[31];        /* x0 to x30 (x30 is the link register) */
    unsigned long sp, pc, pstate;
} mcontext_t;
#endif

typedef struct ucontext {
    struct ucontext *uc_link;       /* NULL: no context to go on with */
    sigset_t         uc_sigmask;    /* the mask sigreturn restores */
    stack_t          uc_stack;      /* the alternate stack */
    mcontext_t       uc_mcontext;   /* the registers */
} ucontext_t;

#ifdef __cplusplus
}
#endif

#endif /* ! _SYS_UCONTEXT_H */
