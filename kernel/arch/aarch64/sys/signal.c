/** \file
 * aarch64: signals on the way back to user mode, and the signal handler's
 * frame.
 *
 * This is the aarch64 side of docs/kernel/signals.md, and works as x86's
 * sys/signal.c does. Every return to user mode goes through
 * `aarch64_return_to_user()`, where the process takes its signals
 * (`signal_take()`). A system call that a signal interrupted is either
 * restarted, by going back to its `svc` with its first argument back in
 * x0 (the result took its place), or fails with `EINTR`. A handler runs by
 * rewriting the user context saved on the kernel stack.
 *
 * The handler's frame, a `struct sigframe` on the user stack (or the
 * alternate stack, with `SA_ONSTACK`), holds the siginfo, the ucontext
 * (the mask to restore and the interrupted registers) and the FP/SIMD
 * state. The handler is called with the signal in x0, the siginfo in x1
 * and the ucontext in x2, and it returns (through x30) into the sigcode
 * page, which makes the `sigreturn` system call.
 */

#include <core/system.h>
#include <core/arch.h>
#include <core/string.h>
#include <cpu/cpu.h>
#include <ds/hashmap.h>
#include <mm/mm.h>
#include <mm/pmap.h>
#include <mm/vm.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <sys/uaccess.h>

#include <aquila/syscall.h>
#include <aquila/ucontext.h>

/* The flags that user code may set: N, Z, C and V. */
#define USER_FLAGS  0xF0000000UL

/* A signal handler's frame, on the user stack. */
struct sigframe {
    struct aquila_siginfo info;
    struct aquila_ucontext uc;  /* the interrupted context, and the mask
                                 * that sigreturn restores */
    uint64_t fpsimd_valid;      /* whether fpsimd holds the thread's state */
    uint8_t fpsimd[FPSIMD_STATE_SIZE] __aligned(16);
} __aligned(16);

/* The sigcode page's code: mov x8, #SYS_sigreturn; svc #0; udf #0. */
static const uint32_t sigcode[] = {
    0xD2800000 | (SYS_sigreturn << 5) | 8,
    0xD4000001,
    0x00000000,
};

/** Tells whether a page is at the offset that `b` points to (for the
 * object's hashmap of pages). */
static int vm_page_eq(void *a, void *b)
{
    return ((struct vm_page *) a)->off == *(size_t *) b;
}

/** Makes the sigcode page, the first time a process touches it. The
 * object keeps it, and every process maps it. Returns NULL if there's no
 * memory. */
static struct vm_page *sigcode_in(struct vm_object *vm_object, size_t off)
{
    char *buf = kmalloc(PAGE_SIZE, &M_BUFFER, M_ZERO);
    struct vm_page *vm_page = buf ? mm_page_alloc() : NULL;

    if (!vm_page) {
        kfree(buf);
        return NULL;
    }

    memcpy(buf, sigcode, sizeof(sigcode));
    pmap_page_write(vm_page->paddr, 0, PAGE_SIZE, buf);
    kfree(buf);

    /* it was written through the direct map, and runs from user mode's
     * mapping: make the instruction cache see it */
    asm volatile("dc cvau, %0; dsb ish; ic ivau, %0; dsb ish; isb"
                 :: "r"(PHYS2VIRT(vm_page->paddr)) : "memory");

    vm_page->vm_object = vm_object;
    vm_page->off = 0;
    vm_page->ref = 1;

    if (vm_object_page_insert(vm_object, vm_page)) {
        mm_page_dealloc(vm_page->paddr);
        return NULL;
    }

    return vm_page;
}

static struct vm_pager sigcode_pager = {
    .in = sigcode_in,
};

static struct vm_object sigcode_object = {
    .pager = &sigcode_pager,
    .ref   = 1,
};

/*
 * Maps the sigcode page into a process's new address space.
 */
int arch_sigcode_map(struct proc *proc)
{
    if (!sigcode_object.pages && !(sigcode_object.pages = hashmap_new(0, vm_page_eq)))
        return -ENOMEM;

    struct vm_entry *vm_entry = kmalloc(sizeof(struct vm_entry), &M_VM_ENTRY, M_ZERO);

    if (!vm_entry)
        return -ENOMEM;

    vm_entry->base  = SIGCODE_ADDR;
    vm_entry->size  = PAGE_SIZE;
    vm_entry->flags = VM_URX;
    vm_entry->vm_object = &sigcode_object;

    int err = vm_space_insert(&proc->vm_space, vm_entry);

    if (err) {
        kfree(vm_entry);
        return err;
    }

    vm_object_incref(&sigcode_object);
    return 0;
}

/** Sets up the user context `r` so that the handler of `sig` runs when
 * it's returned to. A frame that can't be written ends the process with
 * `SIGSEGV`. */
static void deliver(struct aarch64_regs *r, int sig, const struct aquila_siginfo *info)
{
    struct thread *thread = curthread;
    struct proc *proc = curproc;
    struct aquila_sigaction *sa = &proc->sigaction[sig];

    /* the mask to come back to: for `sigsuspend()` and `pselect()`, it's
     * the one they replaced */
    sigset_t mask = thread->sig_oldmask_set ? thread->sig_oldmask : thread->sigmask;
    thread->sig_oldmask_set = 0;

    /* on the alternate stack, from its top, or below where the thread
     * was */
    uintptr_t sp = signal_altstack_top(thread, sa, r->sp);

    if (!sp)
        sp = r->sp;

    uintptr_t f = (sp - sizeof(struct sigframe)) & ~(uintptr_t) 15;

    /* make the frame here, zeroed so that nothing of the kernel's goes
     * out */
    struct sigframe frame;
    struct sigframe *uframe = (struct sigframe *) f;
    memset(&frame, 0, sizeof(frame));
    frame.info = *info;
    frame.uc.uc_sigmask = mask;
    signal_altstack_get(thread, r->sp, &frame.uc.uc_stack);
    memcpy(frame.uc.uc_mcontext.x, r->x, sizeof(r->x));
    frame.uc.uc_mcontext.sp = r->sp;
    frame.uc.uc_mcontext.pc = r->pc;
    frame.uc.uc_mcontext.pstate = r->pstate;

    /* save the FP/SIMD registers as they are, since the handler may use
     * them */
    void *fp = aarch64_fpsimd_current(thread);

    if (fp) {
        memcpy(frame.fpsimd, fp, FPSIMD_STATE_SIZE);
        frame.fpsimd_valid = 1;
    }

    /* there's nowhere to put the frame: as with a fault, the process
     * can't go on */
    if (sp < 4096 || copyout(uframe, &frame, sizeof(frame)))
        signal_exit(SIGSEGV);

    r->x[0]  = sig;
    r->x[1]  = (uintptr_t) &uframe->info;
    r->x[2]  = (uintptr_t) &uframe->uc;
    r->x[30] = SIGCODE_ADDR;
    r->pc    = sa->sa_handler;
    r->sp    = f;
    r->pstate &= USER_FLAGS;

    /* while the handler runs, its mask is blocked, and so is the signal
     * itself unless `SA_NODEFER` is set */
    sigset_t block = sa->sa_mask | (sa->sa_flags & SA_NODEFER ? 0 : 1UL << sig);
    thread->sigmask = (mask | block) & ~SIG_UNMASKABLE;

    if (sa->sa_flags & SA_RESETHAND)
        sa->sa_handler = SIG_DFL;
}

/**
 * Takes the current thread's signals on its way back to user mode, and
 * restarts or fails the system call that a signal interrupted.
 *
 * @param r        the user context saved on the kernel stack
 * @param syscall  whether the thread is returning from a system call
 */
void aarch64_return_to_user(struct aarch64_regs *r, int syscall)
{
    struct thread *thread = curthread;

    if (!thread || !thread->arch || !REGS_USER(r))
        return;

    struct proc *proc = thread->owner;
    struct aquila_siginfo info;
    int sig = signal_take(&info);

    /* a system call that a signal interrupted runs again if no handler
     * runs or the handler has `SA_RESTART`, and fails with `EINTR`
     * otherwise */
    if (syscall) {
        intptr_t ret = (intptr_t) r->x[0];

        if (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND) {
            int restart = !sig || (ret == -ERESTARTSYS && (proc->sigaction[sig].sa_flags & SA_RESTART));

            if (restart) {
                /* go back to the `svc`, with its first argument back in
                 * x0 (the number is still in x8) */
                r->pc -= 4;
                r->x[0] = r->orig_x0;

                /* the call is entered again right away, so it may go
                 * on from where it was */
                thread->restarting = !sig;
            } else {
                r->x[0] = -EINTR;
            }
        }
    }

    if (sig) {
        deliver(r, sig, &info);
    } else if (thread->sig_oldmask_set) {
        /* `sigsuspend()` is restarted: put back the mask it replaced */
        thread->sigmask = thread->sig_oldmask;
        thread->sig_oldmask_set = 0;
    }
}

/* Returns from a signal handler, for `sigreturn()`: restores the
 * registers and the mask from its ucontext, and the FP/SIMD state from its
 * frame. A bad frame gets `SIGSEGV`. */
void arch_sigreturn(void)
{
    struct aarch64_thread *arch = curthread->arch;
    struct aarch64_regs *r = arch->regs;
    uintptr_t f = r->sp;

    struct sigframe frame;

    if ((f & 15) || copyin(&frame, (void *) f, sizeof(frame))) {
        signal_force(curthread, SIGSEGV, SI_KERNEL, f);
        return;
    }

    /* restore the ucontext's registers, but at user mode's level, and
     * only the flags that it could set itself */
    struct aquila_mcontext *mc = &frame.uc.uc_mcontext;

    memcpy(r->x, mc->x, sizeof(r->x));
    r->sp = mc->sp;
    r->pc = mc->pc;
    r->pstate = mc->pstate & USER_FLAGS;

    curthread->sigmask = frame.uc.uc_sigmask & ~SIG_UNMASKABLE;
    aarch64_fpsimd_restore(curthread, frame.fpsimd_valid ? frame.fpsimd : NULL);
}

/*
 * Returns the user stack pointer of the current thread, from the context
 * it entered the kernel with.
 */
uintptr_t arch_user_sp(void)
{
    struct aarch64_thread *arch = curthread->arch;
    return arch->regs->sp;
}
