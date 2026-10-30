/** \file
 * x86: the FPU and SSE registers, switched lazily between threads.
 *
 * Saving and restoring the FPU's 512 bytes on every thread switch would
 * be costly, and most threads never use it. So the switch only sets
 * CR0.TS; the first FPU or SSE instruction a thread runs afterwards
 * raises `#NM`, and `x86_fpu_trap()` saves the previous owner's state and
 * loads this thread's. A thread that never touches the FPU never pays for
 * it.
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/assert.h>
#include <core/arch.h>
#include <core/string.h>
#include <cpu/cpu.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signal.h>

MALLOC_DEFINE(M_X86_FPU, "x86-fpu", "x86 FPU context");

#define CPUID_EDX_FXSR  _BV(24)
#define CPUID_EDX_SSE   _BV(25)

/* fxsave and fxrstor need a 16-byte aligned area */
static char fpu_context[512] __aligned(16);
static int fpu_fxsr = 0;

/* the thread whose state the running CPU's FPU holds */
#define last_fpu_thread (curcpu()->arch.fpu_thread)

/* The MXCSR bits the CPU accepts (fxsave's MXCSR_MASK, or 0xFFBF if it
 * says 0). An MXCSR from a signal frame is masked with it, because
 * fxrstor would fault in the kernel on a reserved bit. */
static uint32_t mxcsr_mask = 0xFFBF;

/* The state a thread starts with: fninit's, with MXCSR's default value
 * and the registers zeroed, so that it never sees the last owner's XMM
 * registers. */
static char fpu_clean[512] __aligned(16);

/**
 * Sets up the FPU on the running CPU, at boot: CR0.TS set so that the
 * first FPU or SSE instruction raises `#NM` (`x86_fpu_trap()`), SSE turned
 * on if the CPU has it, and the clean starting state recorded. CR0.EM
 * must stay clear, or SSE instructions would raise `#UD` instead.
 */
void x86_fpu_setup(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    asm volatile("cpuid":"+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

    write_cr0((read_cr0() & ~CR0_EM) | CR0_MP | CR0_NE | CR0_TS);

    if (edx & CPUID_EDX_FXSR) {
        fpu_fxsr = 1;
        uintptr_t cr4 = read_cr4() | CR4_OSFXSR;

        if (edx & CPUID_EDX_SSE)
            cr4 |= CR4_OSXMMEXCPT;

        write_cr4(cr4);

        /* record a clean state, and the MXCSR mask, from the FPU */
        x86_fpu_enable();
        asm volatile("fninit");
        asm volatile("fxsave (%0)"::"r"(fpu_clean):"memory");
        x86_fpu_disable();

        memset(fpu_clean + 32, 0, sizeof(fpu_clean) - 32);  /* the data registers */
        *(uint32_t *) (fpu_clean + 24) = 0x1F80;            /* MXCSR */

        uint32_t mask = *(uint32_t *) (fpu_clean + 28);
        if (mask)
            mxcsr_mask = mask;
    }
}

/** Lets the running CPU use the FPU without trapping (clears CR0.TS). */
void x86_fpu_enable(void)
{
    asm volatile("clts");
}

/** Makes the next FPU instruction trap (sets CR0.TS). */
void x86_fpu_disable(void)
{
    write_cr0(read_cr0() | CR0_TS);
}

/** Loads the clean starting state into the FPU, for a thread's first use
 * of it. */
void x86_fpu_init(void)
{
    assert_alignof(fpu_context, 16);

    if (fpu_fxsr) {
        memcpy(fpu_context, fpu_clean, 512);
        asm volatile("fxrstor (%0)"::"r"(fpu_context):"memory");
    } else {
        asm volatile("fninit");
    }
}

/** Saves the FPU's state into `fpu_context`. */
static inline void fpu_save(void)
{
    if (fpu_fxsr)
        asm volatile("fxsave (%0)"::"r"(fpu_context):"memory");
    else
        asm volatile("fnsave (%0)"::"r"(fpu_context):"memory");
}

/** Loads the FPU's state from `fpu_context`. */
static inline void fpu_restore(void)
{
    if (fpu_fxsr)
        asm volatile("fxrstor (%0)"::"r"(fpu_context):"memory");
    else
        asm volatile("frstor (%0)"::"r"(fpu_context):"memory");
}

/**
 * Handles `#NM`, the current thread's first FPU or SSE instruction since
 * it was switched in: it saves the previous owner's state, and gives the
 * FPU to this thread, with its own state or a clean one. If there's no
 * memory for the thread's save area, the process is killed.
 */
void x86_fpu_trap(void)
{
    if (!curthread)
        panic("FPU trap without a current thread");

    struct x86_thread *arch = curthread->arch;

    /* Allocate the thread's save area on its first use of the FPU, so
     * that saving it when switching away later can never fail. */
    if (!arch->fpu_context) {
        arch->fpu_context = kmalloc(512, &M_X86_FPU, 0);

        if (!arch->fpu_context) {
            /* it can't have the FPU: the process is killed */
            signal_proc_send(curproc, SIGKILL);
            return;
        }
    }

    x86_fpu_enable();

    /* the FPU has this thread's state already */
    if (curthread == last_fpu_thread)
        return;

    if (last_fpu_thread) {  /* save the previous owner's state */
        struct x86_thread *_arch = last_fpu_thread->arch;
        fpu_save();
        memcpy(_arch->fpu_context, fpu_context, 512);
    }

    if (arch->fpu_enabled) {    /* load this thread's state */
        memcpy(fpu_context, arch->fpu_context, 512);
        fpu_restore();
    } else {                    /* its first use: a clean state */
        x86_fpu_init();
        arch->fpu_enabled = 1;
    }

    last_fpu_thread = curthread;
}

/**
 * Says which floating-point exception made the current thread fault, for
 * its `SIGFPE`'s `si_code`: from the status of the x87 FPU (`#MF`) or of
 * SSE (`#XM`), the exceptions that are flagged and not masked. The FPU
 * holds the thread's state: it just ran one of its instructions.
 *
 * @param vec  the exception: 0x10 (`#MF`) or 0x13 (`#XM`)
 * @return `FPE_FLTINV`, `FPE_FLTDIV`, `FPE_FLTOVF`, `FPE_FLTUND` or
 *         `FPE_FLTRES`, or `SI_KERNEL` if none is flagged
 */
int x86_fpu_fault_code(unsigned vec)
{
    unsigned status;

    if (curthread != last_fpu_thread)
        return SI_KERNEL;

    if (vec == 0x13) {
        /* MXCSR: the flags in bits 0-5, their masks in bits 7-12 */
        uint32_t mxcsr;
        asm volatile("stmxcsr %0" : "=m"(mxcsr));
        status = mxcsr & ~(mxcsr >> 7);
    } else {
        /* the status word's flags, the control word's masks, both in
         * bits 0-5 */
        uint16_t sw, cw;
        asm volatile("fnstsw %0; fnstcw %1" : "=m"(sw), "=m"(cw));
        status = sw & ~cw;
    }

    /* invalid, denormal, zero divide, overflow, underflow, precision */
    if (status & 0x01) return FPE_FLTINV;
    if (status & 0x04) return FPE_FLTDIV;
    if (status & 0x08) return FPE_FLTOVF;
    if (status & 0x12) return FPE_FLTUND;
    if (status & 0x20) return FPE_FLTRES;

    return SI_KERNEL;
}

/**
 * Saves the current thread's FPU state when it is switched out, if the FPU
 * holds it, because the thread may run on another CPU next. Afterwards,
 * the FPU belongs to no thread.
 */
void x86_fpu_switch_out(void)
{
    if (!curthread || last_fpu_thread != curthread)
        return;

    struct x86_thread *arch = curthread->arch;

    x86_fpu_enable();
    fpu_save();
    memcpy(arch->fpu_context, fpu_context, 512);
    last_fpu_thread = NULL;
}

/**
 * Copies the current thread's FPU state, for a signal handler's frame
 * (`sys/signal.c`).
 *
 * @param buf  where to copy it: 512 bytes, 16-byte aligned
 * @return 1 if the state was copied, or 0 if the thread never used the FPU
 */
int x86_fpu_save_to(void *buf)
{
    struct x86_thread *arch = curthread->arch;

    if (!arch->fpu_enabled)
        return 0;

    if (last_fpu_thread == curthread) {
        x86_fpu_enable();
        fpu_save();
        memcpy(buf, fpu_context, 512);
    } else {
        memcpy(buf, arch->fpu_context, 512);
    }

    return 1;
}

/** Makes the current thread start with a clean FPU state at its next use
 * of the FPU, as a signal handler does. */
void x86_fpu_reset(void)
{
    struct x86_thread *arch = curthread->arch;

    arch->fpu_enabled = 0;

    if (last_fpu_thread == curthread)
        last_fpu_thread = NULL;

    x86_fpu_disable();
}

/**
 * Gives the current thread back the FPU state saved in a signal frame, at
 * `sigreturn`. It is loaded at the thread's next use of the FPU, with the
 * MXCSR bits the CPU doesn't accept cleared.
 *
 * @param buf    the saved state: 512 bytes
 * @param valid  whether the frame has a state (the thread had used the
 *               FPU); if not, the thread starts afresh
 */
void x86_fpu_load_from(const void *buf, int valid)
{
    struct x86_thread *arch = curthread->arch;

    x86_fpu_reset();

    if (!valid)
        return;

    if (!arch->fpu_context && !(arch->fpu_context = kmalloc(512, &M_X86_FPU, 0)))
        return;

    memcpy(arch->fpu_context, buf, 512);

    if (fpu_fxsr)
        *(uint32_t *) ((char *) arch->fpu_context + 24) &= mxcsr_mask;

    arch->fpu_enabled = 1;
}
