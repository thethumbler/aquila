/** \file
 * x86: exceptions and system calls.
 *
 * The stubs in `cpu.S` call `__x86_isr()` for the 32 exception vectors
 * and for `int $0x80`, the system call. A page fault goes to the memory
 * code, `#NM` to the FPU code, a system call to `arch_syscall()`. Any
 * other exception from user mode becomes a signal to the process; from
 * the kernel, it is a bug, and the kernel panics.
 */

#include <core/system.h>
#include <core/arch.h>

#include <core/panic.h>
#include <core/string.h>
#include <cpu/cpu.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <sys/lock.h>
#include <mm/mm.h>

extern void __x86_isr0 (void);
extern void __x86_isr1 (void);
extern void __x86_isr2 (void);
extern void __x86_isr3 (void);
extern void __x86_isr4 (void);
extern void __x86_isr5 (void);
extern void __x86_isr6 (void);
extern void __x86_isr7 (void);
extern void __x86_isr8 (void);
extern void __x86_isr9 (void);
extern void __x86_isr10(void);
extern void __x86_isr11(void);
extern void __x86_isr12(void);
extern void __x86_isr13(void);
extern void __x86_isr14(void);
extern void __x86_isr15(void);
extern void __x86_isr16(void);
extern void __x86_isr17(void);
extern void __x86_isr18(void);
extern void __x86_isr19(void);
extern void __x86_isr20(void);
extern void __x86_isr21(void);
extern void __x86_isr22(void);
extern void __x86_isr23(void);
extern void __x86_isr24(void);
extern void __x86_isr25(void);
extern void __x86_isr26(void);
extern void __x86_isr27(void);
extern void __x86_isr28(void);
extern void __x86_isr29(void);
extern void __x86_isr30(void);
extern void __x86_isr31(void);
extern void __x86_isr128(void);

/* The exceptions' names: see the Intel 64 and IA-32 Architectures
 * Software Developer's Manual, Volume 3 (System Programming Guide), Table
 * 6-1, "Protected-Mode Exceptions and Interrupts". */

static const char *int_msg[32] = {
    /* 0x00 */ "#DE: Divide Error",
    /* 0x01 */ "#DB: Debug Exception",
    /* 0x02 */ "NMI Interrupt",
    /* 0x03 */ "#BP: Breakpoint",
    /* 0x04 */ "#OF: Overflow",
    /* 0x05 */ "#BR: BOUND Range Exceeded",
    /* 0x06 */ "#UD: Invalid Opcode (Undefined Opcode)",
    /* 0x07 */ "#NM: Device Not Available (No Math Coprocessor)",
    /* 0x08 */ "#DF: Double Fault",
    /* 0x09 */ "Coprocessor Segment Overrun (reserved)",
    /* 0x0a */ "#TS: Invalid TSS",
    /* 0x0b */ "#NP: Segment Not Present",
    /* 0x0C */ "#SS: Stack-Segment Fault",
    /* 0x0D */ "#GP: General Protection",
    /* 0x0E */ "#PF: Page Fault",
    /* 0x0F */ "Reserved",
    /* 0x10 */ "#MF: x87 FPU Floating-Point Error (Math Fault)",
    /* 0x11 */ "#AC: Alignment Check",
    /* 0x12 */ "#MC: Machine Check",
    /* 0x13 */ "#XM: SIMD Floating-Point Exception",
    /* 0x14 */ "#VE: Virtualization Exception",
    /* 0x15 */ "Reserved",
    /* 0x16 */ "Reserved",
    /* 0x17 */ "Reserved",
    /* 0x18 */ "Reserved",
    /* 0x19 */ "Reserved",
    /* 0x1A */ "Reserved",
    /* 0x1B */ "Reserved",
    /* 0x1C */ "Reserved",
    /* 0x1D */ "Reserved",
    /* 0x1E */ "Reserved",
    /* 0x1F */ "Reserved"
};

/* The signal that an exception in user mode sends to the process, or 0
 * if the exception is the kernel's to handle (or fatal). */
static const int int_sig[32] = {
    /* 0x00 */ SIGFPE,  /* #DE */
    /* 0x01 */ SIGTRAP, /* #DB */
    /* 0x02 */ 0,
    /* 0x03 */ SIGTRAP, /* #BP */
    /* 0x04 */ SIGSEGV, /* #OF */
    /* 0x05 */ SIGSEGV, /* #BR */
    /* 0x06 */ SIGILL,  /* #UD */
    /* 0x07 */ 0,
    /* 0x08 */ 0,
    /* 0x09 */ SIGFPE,
    /* 0x0A */ 0,
    /* 0x0B */ SIGBUS,  /* #NP */
    /* 0x0C */ SIGBUS,  /* #SS */
    /* 0x0D */ SIGSEGV, /* #GP */
    /* 0x0E */ SIGSEGV, /* #PF */
    /* 0x0F */ 0,
    /* 0x10 */ SIGFPE,  /* #MF */
    /* 0x11 */ SIGBUS,  /* #AC */
    /* 0x12 */ 0,
    /* 0x13 */ SIGFPE,  /* #XM */
};

/** Handles a fault in one of the user memory copy routines (`uaccess.S`):
 * the routine resumes at its fixup, which reports the failure to its
 * caller. Returns 1 if the faulting instruction was one of them, 0 if it
 * wasn't. */
static int uaccess_fixup(struct x86_regs *regs)
{
    extern uintptr_t x86_uaccess_fixups[], x86_uaccess_fixups_end[];
#if ARCH_BITS==32
    uintptr_t ip = regs->eip;
#else
    uintptr_t ip = regs->rip;
#endif

    for (uintptr_t *f = x86_uaccess_fixups; f < x86_uaccess_fixups_end; f += 2) {
        if (ip == f[0]) {
#if ARCH_BITS==32
            regs->eip = f[1];
#else
            regs->rip = f[1];
#endif
            return 1;
        }
    }

    return 0;
}

/** Handles an exception or a system call, by its vector. */
static void isr_dispatch(struct x86_regs *regs)
{
    unsigned vec = regs->vec;

    if (vec == 0xE && curthread) { /* a page fault */
        uintptr_t addr = read_cr2();

        if (!arch_mm_page_fault(addr, regs->err)) {
            x86_return_to_user(regs, 0);
            return;
        }

        /* one from user mode that can't be resolved: SIGSEGV, delivered
         * on the way back */
        if ((regs->cs & 3) == 3) {
            signal_force(curthread, SIGSEGV);
            x86_return_to_user(regs, 0);
            return;
        }

        /* one from the kernel, copying from or to user memory: the copy
         * fails with -EFAULT (a fault on the kernel's side of the copy is
         * a bug, as anywhere else) */
        if (addr < USER_STACK && uaccess_fixup(regs))
            return;

        /* one from the kernel anywhere else, even at a user address, is a
         * bug: the kernel only touches user memory through copyin() and
         * copyout() */
        printk("kernel: page fault at %p%s\n", (void *) addr,
               addr < USER_STACK ? " (user memory, outside copyin/copyout)" : "");
        x86_dump_registers(regs);
        panic("page fault in the kernel");
    }

    if (vec == 0x07) {  /* #NM: the thread's first FPU use since its switch */
        x86_fpu_trap();
        x86_return_to_user(regs, 0);
        return;
    }
    
    if (vec == 0x80) {  /* a system call */
        if (!curthread)
            panic("syscall without a current thread");

        struct x86_thread *arch = curthread->arch;
        arch->regs = regs;
        arch_syscall(regs);
        return;
    }


    /* an exception caused by user code: a signal to the process */
    if (vec < 32 && (regs->cs & 3) == 3 && curthread
            && int_sig[vec]) {
        struct x86_thread *arch = curthread->arch;
        arch->regs = regs;

        /* the faulting instruction */
#if ARCH_BITS==32
        curthread->fault_addr = regs->eip;
#else
        curthread->fault_addr = regs->rip;
#endif
        signal_force(curthread, int_sig[vec]);
        x86_return_to_user(regs, 0);
        return;
    }

    if (vec < 32) {
        const char *msg = int_msg[vec];
        printk("Recieved interrupt %d [err=%d]: %s\n", vec, (int) regs->err, msg);

        if (vec == 0x0E) { /* a page fault */
            printk("CR2 = %p\n", read_cr2());
        }
        x86_dump_registers(regs);
        panic("Kernel Exception");
    } else {
        printk("Unhandled interrupt %d\n", vec);
        panic("Kernel Exception");
    }
}

/**
 * Handles an exception or a system call, which the stubs in `cpu.S` call.
 * It takes the kernel lock if it came from user mode (an exception in the
 * kernel holds it already), and lets go of it on the way back.
 *
 * @param regs  the interrupted registers, saved on the kernel stack
 */
void __x86_isr(struct x86_regs *regs)
{
    int took = kernel_enter();

    /* its process was killed while it ran (from another CPU): the thread
     * ends now, and what it came in for is dropped */
    if (took && curthread && curthread->killed)
        sched_thread_killed();

    isr_dispatch(regs);
    kernel_leave(took);
}

/** Sets the IDT's gates for the exceptions and the system call vector. */
void x86_isr_setup(void)
{   
    x86_idt_gate_set(0x00, (uintptr_t) __x86_isr0);
    x86_idt_gate_set(0x01, (uintptr_t) __x86_isr1);
    x86_idt_gate_set(0x02, (uintptr_t) __x86_isr2);
    x86_idt_gate_set(0x03, (uintptr_t) __x86_isr3);
    x86_idt_gate_set(0x04, (uintptr_t) __x86_isr4);
    x86_idt_gate_set(0x05, (uintptr_t) __x86_isr5);
    x86_idt_gate_set(0x06, (uintptr_t) __x86_isr6);
    x86_idt_gate_set(0x07, (uintptr_t) __x86_isr7);
    x86_idt_gate_set(0x08, (uintptr_t) __x86_isr8);
    x86_idt_gate_set(0x09, (uintptr_t) __x86_isr9);
    x86_idt_gate_set(0x0A, (uintptr_t) __x86_isr10);
    x86_idt_gate_set(0x0B, (uintptr_t) __x86_isr11);
    x86_idt_gate_set(0x0C, (uintptr_t) __x86_isr12);
    x86_idt_gate_set(0x0D, (uintptr_t) __x86_isr13);
    x86_idt_gate_set(0x0E, (uintptr_t) __x86_isr14);
    x86_idt_gate_set(0x0F, (uintptr_t) __x86_isr15);
    x86_idt_gate_set(0x10, (uintptr_t) __x86_isr16);
    x86_idt_gate_set(0x11, (uintptr_t) __x86_isr17);
    x86_idt_gate_set(0x12, (uintptr_t) __x86_isr18);
    x86_idt_gate_set(0x13, (uintptr_t) __x86_isr19);
    x86_idt_gate_set(0x14, (uintptr_t) __x86_isr20);
    x86_idt_gate_set(0x15, (uintptr_t) __x86_isr21);
    x86_idt_gate_set(0x16, (uintptr_t) __x86_isr22);
    x86_idt_gate_set(0x17, (uintptr_t) __x86_isr23);
    x86_idt_gate_set(0x18, (uintptr_t) __x86_isr24);
    x86_idt_gate_set(0x19, (uintptr_t) __x86_isr25);
    x86_idt_gate_set(0x1A, (uintptr_t) __x86_isr26);
    x86_idt_gate_set(0x1B, (uintptr_t) __x86_isr27);
    x86_idt_gate_set(0x1C, (uintptr_t) __x86_isr28);
    x86_idt_gate_set(0x1D, (uintptr_t) __x86_isr29);
    x86_idt_gate_set(0x1E, (uintptr_t) __x86_isr30);
    x86_idt_gate_set(0x1F, (uintptr_t) __x86_isr31);
    x86_idt_gate_user_set(0x80, (uintptr_t) __x86_isr128);
}
