/** \file
 * x86: signals, on the way back to user mode, and a handler's frame.
 *
 * Every return to user mode (from a system call, an interrupt or an
 * exception) goes through `x86_return_to_user()`. There the process takes
 * its signals (`signal_take()`: stops, default actions), a system call
 * that a signal interrupted is restarted or fails with `EINTR`, and a
 * handler is made to run by rewriting the user context saved on the
 * kernel stack. Signals come nowhere else, so the kernel never leaves
 * anything halfway.
 *
 * A handler's frame, on the user stack (or its alternate stack, with
 * `SA_ONSTACK`), is what the handler returns to (the sigcode page) and
 * its arguments (in registers on x86_64), then a `struct sigframe`: the
 * siginfo, the ucontext (the mask to restore and the interrupted
 * registers) and the FPU state.
 *
 * The handler returns into the sigcode page, mapped in every process
 * (`SIGCODE_ADDR`, shared, read and execute), which makes the `sigreturn`
 * system call. It reads the frame back, takes the registers from the
 * ucontext (with user segments, and no privileged flags), and restores
 * the mask and the FPU state.
 *
 * The frame is made in the kernel and written with `copyout()`, and read
 * back with `copyin()`: a stack that isn't there is the process's fault
 * (`SIGSEGV`), not the kernel's.
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

#if ARCH_BITS==32
#define REG_IP(r)   ((r)->eip)
#define REG_SP(r)   ((r)->esp)
#define REG_AX(r)   ((r)->eax)
#define REG_FLAGS(r) ((r)->eflags)
#else
#define REG_IP(r)   ((r)->rip)
#define REG_SP(r)   ((r)->rsp)
#define REG_AX(r)   ((r)->rax)
#define REG_FLAGS(r) ((r)->rflags)
#endif

/* the EFLAGS bits that user code may set: the arithmetic ones, TF, DF,
 * AC and ID (not IF, IOPL, NT, VM, ...) */
#define USER_FLAGS  0x00240DD5UL

/** A signal handler's frame, on the user stack. */
struct sigframe {
    struct aquila_siginfo info;
    struct aquila_ucontext uc;  /* the interrupted context, and the mask
                                 * that sigreturn restores */
    uint32_t fpu;               /* fxsave below has the thread's FPU state */
    uint8_t fxsave[512] __aligned(16);
} __aligned(16);

/** Copies the registers user code sees, from a saved context to a
 * ucontext's, and back. */
static void regs_to_mcontext(const struct x86_regs *r, struct aquila_mcontext *mc)
{
#if ARCH_BITS==32
    *mc = (struct aquila_mcontext) {
        .eax = r->eax, .ebx = r->ebx, .ecx = r->ecx, .edx = r->edx,
        .esi = r->esi, .edi = r->edi, .ebp = r->ebp, .esp = r->esp,
        .eip = r->eip, .eflags = r->eflags,
    };
#else
    *mc = (struct aquila_mcontext) {
        .rax = r->rax, .rbx = r->rbx, .rcx = r->rcx, .rdx = r->rdx,
        .rsi = r->rsi, .rdi = r->rdi, .rbp = r->rbp, .rsp = r->rsp,
        .r8  = r->r8,  .r9  = r->r9,  .r10 = r->r10, .r11 = r->r11,
        .r12 = r->r12, .r13 = r->r13, .r14 = r->r14, .r15 = r->r15,
        .rip = r->rip, .rflags = r->rflags,
    };
#endif
}

static void mcontext_to_regs(const struct aquila_mcontext *mc, struct x86_regs *r)
{
#if ARCH_BITS==32
    r->eax = mc->eax; r->ebx = mc->ebx; r->ecx = mc->ecx; r->edx = mc->edx;
    r->esi = mc->esi; r->edi = mc->edi; r->ebp = mc->ebp; r->esp = mc->esp;
    r->eip = mc->eip; r->eflags = mc->eflags;
#else
    r->rax = mc->rax; r->rbx = mc->rbx; r->rcx = mc->rcx; r->rdx = mc->rdx;
    r->rsi = mc->rsi; r->rdi = mc->rdi; r->rbp = mc->rbp; r->rsp = mc->rsp;
    r->r8  = mc->r8;  r->r9  = mc->r9;  r->r10 = mc->r10; r->r11 = mc->r11;
    r->r12 = mc->r12; r->r13 = mc->r13; r->r14 = mc->r14; r->r15 = mc->r15;
    r->rip = mc->rip; r->rflags = mc->rflags;
#endif
}

/* On i386, below the frame: the return address and the handler's
 * arguments (signal, siginfo, ucontext), padded so that the handler is
 * entered as a call would enter it (the stack 16-byte aligned before the
 * call). */
#define I386_ARGS   20

/*
 * The sigcode page's code: mov $SYS_sigreturn, %eax; int $0x80; ud2
 */
static const uint8_t sigcode[] = {
    0xB8, SYS_sigreturn & 0xff, (SYS_sigreturn >> 8) & 0xff, 0, 0,
    0xCD, 0x80,
    0x0F, 0x0B,
};

/** Compares a page with an offset, for the sigcode object's hashmap. */
static int vm_page_eq(void *a, void *b)
{
    return ((struct vm_page *) a)->off == *(size_t *) b;
}

/** Makes the sigcode page, the first time a process touches it, into
 * `*page`: 0, or `-ENOMEM`. The object keeps it, and every process maps
 * the same page. */
static int sigcode_in(struct vm_object *vm_object, size_t off, struct vm_page **page)
{
    char *buf = kmalloc(PAGE_SIZE, &M_BUFFER, M_ZERO);
    struct vm_page *vm_page = buf ? mm_page_alloc() : NULL;

    if (!vm_page) {
        kfree(buf);
        return -ENOMEM;
    }

    memcpy(buf, sigcode, sizeof(sigcode));
    pmap_page_write(vm_page->paddr, 0, PAGE_SIZE, buf);
    kfree(buf);

    vm_page->vm_object = vm_object;
    vm_page->off = 0;
    vm_page->ref = 1;

    if (vm_object_page_insert(vm_object, vm_page)) {
        mm_page_dealloc(vm_page->paddr);
        return -ENOMEM;
    }

    *page = vm_page;
    return 0;
}

static struct vm_pager sigcode_pager = {
    .in = sigcode_in,
};

static struct vm_object sigcode_object = {
    .pager = &sigcode_pager,
    .ref   = 1,
};

/*
 * Maps the sigcode page in a process's new address space.
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
    vm_entry->vm_space = &proc->vm_space;

    int err = vm_space_insert(&proc->vm_space, vm_entry);

    if (err) {
        kfree(vm_entry);
        return err;
    }

    vm_object_map(&sigcode_object, vm_entry);
    return 0;
}

/** Makes the handler of `sig` run when the context `r` is returned to:
 * writes its frame on the user stack (or the alternate stack), and points
 * `r` at the handler. */
static void deliver(struct x86_regs *r, int sig, const struct aquila_siginfo *info)
{
    struct thread *thread = curthread;
    struct proc *proc = curproc;
    struct aquila_sigaction *sa = &proc->sigaction[sig];
    uintptr_t handler = sa->sa_handler;

    /* the mask to restore afterwards; for sigsuspend() and pselect(),
     * it is the one they replaced */
    sigset_t mask = thread->sig_oldmask_set ? thread->sig_oldmask : thread->sigmask;
    thread->sig_oldmask_set = 0;

    /* on the alternate stack, from its top; or below where the thread
     * was, and on x86_64 below the red zone, the 128 bytes the code may
     * use below its stack pointer */
    uintptr_t sp = signal_altstack_top(thread, sa, REG_SP(r));

    if (!sp)
        sp = REG_SP(r) - (ARCH_BITS == 64 ? 128 : 0);

    uintptr_t f = (sp - sizeof(struct sigframe)) & ~(uintptr_t) 15;

#if ARCH_BITS==32
    uintptr_t new_sp = f - I386_ARGS;
#else
    uintptr_t new_sp = f - sizeof(uintptr_t);
#endif

    /* make the frame here, zeroed, so that nothing of the kernel's goes
     * out */
    struct sigframe frame;
    struct sigframe *uframe = (struct sigframe *) f;

    memset(&frame, 0, sizeof(frame));
    frame.info = *info;
    frame.uc.uc_sigmask = mask;
    signal_altstack_get(thread, REG_SP(r), &frame.uc.uc_stack);
    regs_to_mcontext(r, &frame.uc.uc_mcontext);
    frame.fpu  = x86_fpu_save_to(frame.fxsave);

    /* below it, what the handler returns to (and its arguments) */
#if ARCH_BITS==32
    uintptr_t args[I386_ARGS / sizeof(uintptr_t)] = {
        SIGCODE_ADDR, sig, (uintptr_t) &uframe->info, (uintptr_t) &uframe->uc, 0,
    };
#else
    uintptr_t args[1] = {SIGCODE_ADDR};
#endif

    /* if there's nowhere to put it, the process can't go on, as after a
     * fault */
    if (sp < 4096 || copyout(uframe, &frame, sizeof(frame)) ||
        copyout((void *) new_sp, args, sizeof(args)))
        signal_exit(SIGSEGV);

    /* the handler starts with a clean FPU state */
    x86_fpu_reset();

#if ARCH_BITS==64
    r->rdi = sig;
    r->rsi = (uintptr_t) &uframe->info;
    r->rdx = (uintptr_t) &uframe->uc;
#endif

    REG_IP(r) = handler;
    REG_SP(r) = new_sp;
    REG_FLAGS(r) = (REG_FLAGS(r) & USER_FLAGS & ~0x400UL) | X86_EFLAGS;  /* DF clear */

    /* while it runs, its mask is blocked, and the signal itself (unless
     * SA_NODEFER) */
    sigset_t block = sa->sa_mask | (sa->sa_flags & SA_NODEFER ? 0 : 1UL << sig);
    thread->sigmask = (mask | block) & ~SIG_UNMASKABLE;

    if (sa->sa_flags & SA_RESETHAND)
        sa->sa_handler = SIG_DFL;
}

/**
 * Delivers the current process's pending signals on the way back to user
 * mode: stops and default actions, a handler, and the restart (or
 * `EINTR`) of a system call that a signal interrupted.
 *
 * @param r        the user context about to be returned to
 * @param syscall  whether `r` is returning from a system call (one that
 *                 may need restarting)
 */
void x86_return_to_user(struct x86_regs *r, int syscall)
{
    struct thread *thread = curthread;

    if (!thread || !thread->arch || (r->cs & 3) != 3)
        return;

    struct proc *proc = thread->owner;
    struct aquila_siginfo info;
    int sig = signal_take(&info);

    /* a system call that a signal interrupted is made again (if no
     * handler runs, or the handler says SA_RESTART), or fails with EINTR */
    if (syscall) {
        intptr_t ret = (intptr_t) REG_AX(r);

        if (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND) {
            int restart = !sig || (ret == -ERESTARTSYS && (proc->sigaction[sig].sa_flags & SA_RESTART));

            if (restart) {
                /* back to the int $0x80, with the number in eax again */
                REG_IP(r) -= 2;
                REG_AX(r) = thread->syscall_nr;

                /* straight back into the same call, with the same
                 * arguments: it may go on where it was (if a handler runs
                 * first, its calls aren't the restart) */
                thread->restarting = !sig;
            } else {
                REG_AX(r) = -EINTR;
            }
        }
    }

    if (sig) {
        deliver(r, sig, &info);
    } else if (thread->sig_oldmask_set) {
        /* sigsuspend() restarted: restore the mask it replaced */
        thread->sigmask = thread->sig_oldmask;
        thread->sig_oldmask_set = 0;
    }

    /* an instruction pointer past user space, from a sigreturn frame,
     * can't be returned to (sigaction() checks handlers): on x86_64 one
     * that isn't canonical makes iretq fault in the kernel, with user
     * mode's GS */
    if (REG_IP(r) >= UACCESS_LIMIT)
        signal_exit(SIGSEGV);
}

/* Returns from a signal handler (`sigreturn`): restores the interrupted
 * context and the mask from the handler's ucontext, and the FPU state
 * from its frame. A bad frame raises `SIGSEGV`. */
void arch_sigreturn(void)
{
    struct x86_thread *arch = curthread->arch;
    struct x86_regs *r = arch->regs;

#if ARCH_BITS==32
    uintptr_t f = REG_SP(r) + I386_ARGS - sizeof(uintptr_t);
#else
    uintptr_t f = REG_SP(r);
#endif

    struct sigframe frame;

    if ((f & 15) || copyin(&frame, (void *) f, sizeof(frame))) {
        signal_force(curthread, SIGSEGV, SI_KERNEL, f);
        return;
    }

    /* the ucontext's registers, but with user mode's segments, and no
     * flags that user mode couldn't set itself */
    mcontext_to_regs(&frame.uc.uc_mcontext, r);
    REG_FLAGS(r) = (REG_FLAGS(r) & USER_FLAGS) | X86_EFLAGS;
    r->cs = X86_CS;
    r->ss = X86_SS;

#if ARCH_BITS==32
    r->ds = r->es = r->fs = r->gs = X86_SS;
#endif

    curthread->sigmask = frame.uc.uc_sigmask & ~SIG_UNMASKABLE;
    x86_fpu_load_from(frame.fxsave, frame.fpu);
}

/*
 * Returns the user stack pointer of the current thread, from the context
 * it entered the kernel with.
 */
uintptr_t arch_user_sp(void)
{
    struct x86_thread *arch = curthread->arch;
    return REG_SP(arch->regs);
}
