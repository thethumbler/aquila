#ifndef _AQUILA_UCONTEXT_H
#define _AQUILA_UCONTEXT_H

#include <aquila/signal.h>

/*
 * A signal handler's third argument (the C library's ucontext_t): the
 * context the signal interrupted, in the handler's frame on the user
 * stack. When the handler returns, sigreturn puts back the mask and the
 * registers from here, so a handler that changes them changes where and
 * how the program goes on. The floating-point state is in the frame too,
 * but not here.
 */

/* the registers: uc_mcontext, the C library's mcontext_t */
#if defined(__x86_64__)
struct aquila_mcontext {
    unsigned long rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
    unsigned long rip, rflags;
};
#elif defined(__i386__)
struct aquila_mcontext {
    unsigned long eax, ebx, ecx, edx, esi, edi, ebp, esp;
    unsigned long eip, eflags;
};
#elif defined(__aarch64__)
struct aquila_mcontext {
    unsigned long x[31];        /* x0 to x30 (x30 is the link register) */
    unsigned long sp, pc, pstate;
};
#endif

struct aquila_ucontext {
    unsigned long          uc_link;     /* 0: no context to go on with */
    unsigned long          uc_sigmask;  /* the mask sigreturn restores */
    struct aquila_stack    uc_stack;    /* the alternate stack, as
                                         * sigaltstack() would say */
    struct aquila_mcontext uc_mcontext;
};

#endif /* ! _AQUILA_UCONTEXT_H */
