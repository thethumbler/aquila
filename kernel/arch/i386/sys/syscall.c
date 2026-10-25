/** \file
 * x86: system calls. A program makes one with `int $0x80`: the number in
 * eax, the arguments in ebx, ecx, edx, esi and edi (i386), or in rdi,
 * rsi, rdx, r10 and r8 (x86_64, as with Linux's `syscall` instruction),
 * and the result comes back in eax. See docs/kernel/syscalls.md.
 */

#include <core/system.h>
#include <core/arch.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/syscall.h>
#include <sys/signal.h>
#include <aquila/syscall.h>
#include <aquila/errno.h>

/*
 * Runs a system call: the interrupt handler calls it for `int $0x80`. It
 * returns to user mode when the call is done, delivering the signals that
 * came meanwhile.
 */
void arch_syscall(struct x86_regs *r)
{
#if ARCH_BITS==32
    syscall_dispatch(r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi);
#else
    /* the arguments in Linux's registers (r10 instead of rcx, which the
     * syscall instruction overwrites) */
    syscall_dispatch(r->rax, r->rdi, r->rsi, r->rdx, r->r10, r->r8);
#endif

    /* if the call woke up an urgent thread (the network's, for a packet
     * sent to ourselves), it runs before we return to user mode */
    if (sched_urgent) {
        extern void x86_preempt(void);
        x86_preempt();
    }

    /* deliver the pending signals, and restart the call if one
     * interrupted it (not sigreturn, whose registers are the frame's) */
    x86_return_to_user(r, curthread->syscall_nr != SYS_sigreturn);
}

/*
 * Sets the result of a thread's system call: eax (rax) when it returns to
 * user mode. That's in its saved registers (for a child of `fork()`, its
 * copy of its parent's), or, for a thread that has never run, where it
 * will start from.
 */
void arch_syscall_return(struct thread *thread, uintptr_t val)
{
    struct x86_thread *arch = thread->arch;

#if ARCH_BITS==32
    if (thread->spawned) arch->regs->eax = val;
    else arch->eax = val;
#else
    if (thread->spawned) arch->regs->rax = val;
    else arch->rax = val;
#endif
}
