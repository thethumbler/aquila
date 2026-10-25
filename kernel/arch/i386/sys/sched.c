/** \file
 * x86: the scheduler's architecture side: the tick, the idle loop, and a
 * thread that sleeps, yields or ends.
 */

#include <core/arch.h>
#include <core/platform.h>
#include <core/time.h>
#include <mm/mm.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/lock.h>

#include "sys.h"
#include <platform/misc.h>

static uint32_t timer_period = 0;
static uint64_t timer_ticks = 0;

/*
 * Reads the CPU's cycle counter, the TSC, for randomness.
 */
uint64_t arch_cycles(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

/** Handles the tick, on every CPU: it charges the running process, asks
 * for a thread switch, and on the boot CPU, also moves the clock on and
 * runs the scheduler's timers. */
static void x86_sched_handler(void *arg __unused)
{
    struct x86_regs *r = curcpu()->arch.irq_regs;

    if (curcpu()->id) {
        sched_cpu_tick(timer_period, (r->cs & 3) == 3);
        x86_irq_tick_switch();
        return;
    }

    /* set the wall clock from the real time clock again, every 2^16 ticks
     * (about 2 minutes) */
    if (timer_ticks && !(timer_ticks & 0xFFFF))
        time_resync();

    ++timer_ticks;
    x86_clock_tick();
    sched_timer_tick(timer_period, (r->cs & 3) == 3);

    /* the running thread goes back to the ready list (with all its
     * registers saved), once the controller is done with the interrupt
     * (irq.c) */
    x86_irq_tick_switch();
}

/* Starts the clock and the tick, and wakes up the other CPUs to start
 * theirs. */
void arch_sched_init(void)
{
    /* first the clock (the TSC or the HPET, else the ticks), then the
     * tick, because the local APIC's timer is calibrated against the
     * clock */
    x86_clock_init();
    timer_period = platform_timer_setup(2000000, x86_sched_handler);
    x86_clock_ticks(timer_period);

    /* the other CPUs start their ticks when they are woken up */
    for (int i = 1; i < cpus_count; ++i)
        arch_cpu_kick(&cpus[i]);
}

/** The idle loop, on the CPU's idle stack. It lets go of the kernel lock;
 * an interrupt takes it, and lets go of it again if it comes back here. */
static void __arch_idle(void)
{
    if (kernel_lock_held())
        kernel_unlock();

    for (;;) {
        asm volatile("sti; hlt; cli;");
    }
}

/* Makes the running CPU idle, on its idle stack. It doesn't return. */
void arch_idle(void)
{
    curthread = NULL;

    uintptr_t esp = VMA(0x100000);
    x86_kernel_stack_set(esp);
    uintptr_t stack = curcpu()->arch.idle_stack;
    extern void x86_goto(uintptr_t eip, uintptr_t ebp, uintptr_t esp) __attribute__((noreturn));
    x86_goto((uintptr_t) __arch_idle, stack, stack);
}

/** Ends the current thread, from the CPU's idle stack, then idles. */
static void __arch_cur_thread_kill(void)
{
    /* A thread that called thread_exit() is a zombie already: it stays
     * on its process's list of threads, and is freed by thread_join(),
     * or when the process exits. Otherwise the process is exiting, and
     * proc_kill() took the thread off the list already. */
    int keep = curthread->state == ZOMBIE;

    thread_kill(curthread);

    if (!keep)
        kfree(curthread);

    curthread = NULL;
    kernel_idle();
}

/* Ends the current thread. Its kernel stack is freed, so this switches
 * to the CPU's idle stack first. It doesn't return. */
void arch_cur_thread_kill(void)
{
    uintptr_t stack = curcpu()->arch.idle_stack;

    extern void x86_goto(uintptr_t eip, uintptr_t ebp, uintptr_t esp) __attribute__((noreturn));
    x86_goto((uintptr_t) __arch_cur_thread_kill, stack, stack);
}

/* Puts the current thread at the end of the ready list, and runs the
 * next one. */
void arch_yield(void)
{
    extern void x86_preempt(void);
    x86_preempt();
}

/* Switches away from the current thread, which sleeps: the next ready
 * thread runs, or the CPU idles. It returns when the thread runs again. */
void arch_sleep(void)
{
    extern void x86_sleep(void);
    x86_sleep();
}
