#ifndef _X86_CPU_H
#define _X86_CPU_H

#include <core/system.h>

/* x86: the CPU's structures (the interrupt frame, the GDT and TSS), the
 * control registers, and the functions of arch/i386/cpu/. */

/** The registers an interrupt or exception saved, on the kernel stack,
 * as cpu.S's stubs push them: the general registers, the stub's vector
 * and error code, then what the CPU pushed. */
struct x86_regs {
#if ARCH_BITS==32
    uint32_t
    edi, esi, ebp, ebx, ecx, edx, eax,
    gs, fs, es, ds,
    vec, err,       /* pushed by the stub: the vector, the error code (or 0) */
    eip, cs, eflags, esp, ss;
#else
    uint64_t
    r15, r14, r13, r12, r11, r10, r9, r8,
    rdi, rsi, rbp, rbx, rcx, rdx, rax,
    vec, err,       /* pushed by the stub: the vector, the error code (or 0) */
    rip, cs, rflags, rsp, ss;
#endif
};

/* Prints the saved registers, for a panic. */
static inline void x86_dump_registers(struct x86_regs *regs)
{
    printk("Registers dump:\n");
#if ARCH_BITS==32
    printk("edi = %p\n", regs->edi);
    printk("esi = %p\n", regs->esi);
    printk("ebp = %p\n", regs->ebp);
    printk("ebx = %p\n", regs->ebx);
    printk("ecx = %p\n", regs->ecx);
    printk("edx = %p\n", regs->edx);
    printk("eax = %p\n", regs->eax);
    printk("gs  = %p\n", regs->gs );
    printk("fs  = %p\n", regs->fs );
    printk("es  = %p\n", regs->es );
    printk("ds  = %p\n", regs->ds );
    printk("eip = %p\n", regs->eip);
    printk("cs  = %p\n", regs->cs );
    printk("eflags = %p\n", regs->eflags);
    printk("esp = %p\n", regs->esp);
    printk("ss  = %p\n", regs->ss);
#else
    printk("r15 = %p\n", regs->r15);
    printk("r14 = %p\n", regs->r14);
    printk("r13 = %p\n", regs->r13);
    printk("r12 = %p\n", regs->r12);
    printk("r11 = %p\n", regs->r11);
    printk("r10 = %p\n", regs->r10);
    printk("r9  = %p\n", regs->r9);
    printk("r8  = %p\n", regs->r8);
    printk("rdi = %p\n", regs->rdi);
    printk("rsi = %p\n", regs->rsi);
    printk("rbp = %p\n", regs->rbp);
    printk("rbx = %p\n", regs->rbx);
    printk("rcx = %p\n", regs->rcx);
    printk("rdx = %p\n", regs->rdx);
    printk("rax = %p\n", regs->rax);
    printk("rip = %p\n", regs->rip);
    printk("cs  = %p\n", regs->cs );
    printk("rflags = %p\n", regs->rflags);
    printk("rsp = %p\n", regs->rsp);
    printk("ss  = %p\n", regs->ss);
#endif
}

/** A segment descriptor, an entry of the GDT. */
struct x86_gdt_entry {
    uint32_t limit_lo : 16; /* the limit, bits 0 to 15 */
    uint32_t base_lo  : 16; /* the base, bits 0 to 15 */

    uint32_t base_mid : 8;  /* the base, bits 16 to 23 */
    uint32_t type     : 4;  /* the segment's type */
    uint32_t s        : 1;  /* 0 for a system segment, 1 for code or data */
    uint32_t dpl      : 2;  /* its privilege level */
    uint32_t p        : 1;  /* present */

    uint32_t limit_hi : 4;  /* the limit, bits 16 to 19 */
    uint32_t avl      : 1;  /* free for the system's use */
    uint32_t l        : 1;  /* a 64-bit code segment (x86_64) */
    uint32_t db       : 1;  /* the default operation size (32-bit) */
    uint32_t g        : 1;  /* the limit counts 4 KiB pages, not bytes */
    uint32_t base_hi  : 8;  /* the base, bits 24 to 31 */
} __aligned(8);

/* The task state segment. The kernel only uses it for the stack that
 * interrupts from user mode switch to. */
#if ARCH_BITS==32
struct x86_tss {
    uint32_t link;
    uint32_t sp;
    uint32_t ss;
    uint32_t _[23]; /* the other fields, unused (see Intel's manuals) */
} __packed;
#else
struct x86_tss {
    uint32_t _;
    uint64_t sp;
    uint32_t __[23]; /* the other fields, unused (see Intel's manuals) */
} __packed;
#endif

/*
 * Each CPU has its own GDT, with the same segments but its own TSS (a TSS
 * is busy on the CPU that loaded it) and, on i386, the segment that %gs
 * selects in the kernel: its struct cpu.
 */
#define X86_GDT_TSS     5   /* 5 and 6 on x86_64 */
#define X86_GDT_CPU     7
#define X86_GDT_NR      8

#define X86_SEL_CPU     (X86_GDT_CPU << 3)

struct pmap;
struct thread;

/** A CPU's x86 state (struct cpu's `arch`). */
struct arch_cpu {
    uint32_t apic_id;           /* its local APIC's ID */
    struct x86_gdt_entry gdt[X86_GDT_NR];
    struct x86_tss tss __aligned(8);
    struct pmap *pmap;          /* the page tables in its CR3 (pmap_switch) */
    struct thread *fpu_thread;  /* the thread whose state its FPU holds */
    struct x86_regs *irq_regs;  /* the interrupted context, for the tick */
    int tick_switch;            /* the tick asked for a switch (irq.c) */
    int ticking;                /* its local APIC's timer is running */
    uintptr_t idle_stack;       /* the top of its idle loop's stack */
    uint64_t tlb_flushes;       /* TLB flushes done for another CPU (mm/tlb.c) */
    uint64_t kernel_gen;        /* the generation of the kernel's mappings its TLB has */
};

/* The running CPU's struct cpu. Its first member points to itself, and
 * is at %gs:0: on i386 through the X86_SEL_CPU segment, on x86_64 through
 * the GS base (user mode's is swapped in and out with swapgs). */
struct cpu;

static inline struct cpu *curcpu(void)
{
    struct cpu *cpu;
#if ARCH_BITS==32
    asm volatile("movl %%gs:0, %0" : "=r"(cpu));
#else
    asm volatile("movq %%gs:0, %0" : "=r"(cpu));
#endif
    return cpu;
}

/* CR0's bits */
#define CR0_PG  _BV(31)
#define CR0_WP  _BV(16)     /* read-only pages are read-only for the kernel too */
#define CR0_MP  _BV(1)
#define CR0_EM  _BV(2)
#define CR0_TS  _BV(3)
#define CR0_NE  _BV(5)

/* CR4's bits */
#define CR4_PSE _BV(4)
#define CR4_OSFXSR      _BV(9)
#define CR4_OSXMMEXCPT  _BV(10)

/* Read and write the control registers. */
static inline uintptr_t read_cr0(void)
{
    uint32_t retval = 0;
#if ARCH_BITS==32
    asm volatile("mov %%cr0, %%eax":"=a"(retval));
#else
    asm volatile("movq %%cr0, %%rax":"=a"(retval));
#endif
    return retval;
}

static inline uintptr_t read_cr1(void)
{
    uintptr_t retval = 0;
#if ARCH_BITS==32
    asm volatile("mov %%cr1, %%eax":"=a"(retval));
#else
    asm volatile("movq %%cr1, %%rax":"=a"(retval));
#endif
    return retval;
}

static inline uintptr_t read_cr2(void)
{
    uintptr_t retval = 0;
#if ARCH_BITS==32
    asm volatile("mov %%cr2, %%eax":"=a"(retval));
#else
    asm volatile("movq %%cr2, %%rax":"=a"(retval));
#endif
    return retval;
}

static inline uintptr_t read_cr3(void)
{
    uintptr_t retval = 0;
#if ARCH_BITS==32
    asm volatile("mov %%cr3, %%eax":"=a"(retval));
#else
    asm volatile("movq %%cr3, %%rax":"=a"(retval));
#endif
    return retval;
}

static inline uintptr_t read_cr4(void)
{
    uintptr_t retval = 0;
#if ARCH_BITS==32
    asm volatile("mov %%cr4, %%eax":"=a"(retval));
#else
    asm volatile("movq %%cr4, %%rax":"=a"(retval));
#endif
    return retval;
}

static inline void write_cr0(uintptr_t val)
{
#if ARCH_BITS==32
    asm volatile("mov %%eax, %%cr0"::"a"(val));
#else
    asm volatile("movq %%rax, %%cr0"::"a"(val));
#endif
}

static inline void write_cr1(uintptr_t val)
{
#if ARCH_BITS==32
    asm volatile("mov %%eax, %%cr1"::"a"(val));
#else
    asm volatile("movq %%rax, %%cr1"::"a"(val));
#endif
}

static inline void write_cr2(uintptr_t val)
{
#if ARCH_BITS==32
    asm volatile("mov %%eax, %%cr2"::"a"(val));
#else
    asm volatile("movq %%rax, %%cr2"::"a"(val));
#endif
}

static inline void write_cr3(uintptr_t val)
{
#if ARCH_BITS==32
    asm volatile("mov %%eax, %%cr3"::"a"(val));
#else
    asm volatile("movq %%rax, %%cr3"::"a"(val));
#endif
}

static inline void write_cr4(uintptr_t val)
{
#if ARCH_BITS==32
    asm volatile("mov %%eax, %%cr4"::"a"(val));
#else
    asm volatile("movq %%rax, %%cr4"::"a"(val));
#endif
}

/* cpu/gdt.c */
void x86_gdt_setup(struct cpu *cpu, uintptr_t sp);
void x86_kernel_stack_set(uintptr_t sp);

/* cpu/idt.c */
void x86_idt_setup(void);
void x86_idt_gate_set(uint32_t id, uintptr_t offset);
void x86_idt_gate_user_set(uint32_t id, uintptr_t offset);

/* cpu/isr.c */
void x86_isr_setup(void);

/* cpu/fpu.c */
void x86_fpu_setup(void);
void x86_fpu_enable(void);
void x86_fpu_disable(void);
void x86_fpu_trap(void);
void x86_fpu_switch_out(void);
int  x86_fpu_save_to(void *buf);
void x86_fpu_reset(void);
void x86_fpu_load_from(const void *buf, int valid);
int  x86_fpu_fault_code(unsigned vec);

/* sys/signal.c: delivers the pending signals on the way back to user
 * mode from the context `r` */
void x86_return_to_user(struct x86_regs *r, int syscall);

#include_next <cpu/cpu.h>

#endif /* ! _X86_CPU_H */
