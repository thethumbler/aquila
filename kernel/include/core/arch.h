#ifndef _CORE_ARCH_H
#define _CORE_ARCH_H

/*
 * The functions every architecture provides to the generic kernel, in
 * arch/ARCH/. See docs/kernel/architecture.md for how the kernel and the
 * architecture meet.
 */

#include <core/types.h>
#include <sys/proc.h>
#include <dev/irq.h>

/* arch/ARCH/sys/proc.c */

/**
 * Sets up the architecture's part of a new process's thread (init's, at
 * boot): its kernel stack, and a start at the program's entry point in
 * user mode.
 *
 * @param proc  the process
 */
void arch_proc_init(struct proc *proc);

/**
 * Puts the current thread to sleep. It stops here, and the next ready
 * thread runs, or the CPU idles; it returns once the thread is woken up
 * and chosen to run again.
 */
void arch_sleep(void);

/* arch/ARCH/sys/thread.c */

/**
 * Sets up a new user thread's CPU state and kernel stack, for
 * `pthread_create()`. The thread will start at `entry` in user mode, on
 * its user stack, and that code calls `uentry` with `arg`.
 *
 * @param thread  the new thread
 * @param stack   the top of its user stack
 * @param entry   where it starts in user mode (the C library's thread
 *                start routine)
 * @param uentry  the function that routine calls (the user's function)
 * @param arg     that function's argument
 * @return 0, `-ENOMEM` if there was no memory for the thread's state, or
 *         `-EFAULT` if the stack isn't the process's memory
 */
int  arch_thread_create(struct thread *thread, uintptr_t stack, uintptr_t entry, uintptr_t uentry, uintptr_t arg);

/**
 * Sets up a kernel thread's CPU state. The thread starts at `entry`, a
 * function without arguments that never returns, on its own kernel stack,
 * once it is switched to.
 *
 * @param thread  the new thread; its `arch` stays NULL if there was no
 *                memory for its state
 * @param entry   the function it runs
 */
void arch_kthread_create(struct thread *thread, uintptr_t entry);

/**
 * Frees a thread's CPU state and kernel stack. The thread must not be
 * running on them.
 *
 * @param thread  the thread
 */
void arch_thread_kill(struct thread *thread);

/**
 * Runs a thread for the first time: switches to its address space and
 * kernel stack, and starts it. It doesn't return.
 *
 * @param thread  the thread, which has never run
 */
void arch_thread_spawn(struct thread *thread);

/**
 * Switches to a thread that has run before: its address space, its
 * kernel stack, then where it stopped. It doesn't return.
 *
 * @param thread  the thread
 */
void arch_thread_switch(struct thread *thread);

/** Makes this CPU idle, on its idle stack, until an interrupt makes a
 * thread ready. It doesn't return. */
void arch_idle(void);

/* arch/ARCH/sys/fork.c */

/**
 * Sets up the CPU state of a `fork()` child's thread: a copy of the
 * parent's kernel stack, registers and FPU state, so that the child
 * returns from `fork()` as its parent does.
 *
 * @param thread  the thread that called `fork()`
 * @param fork    the child process, whose one thread gets the state
 * @return 0, or `-ENOMEM` if there was no memory for it
 */
int arch_proc_fork(struct thread *thread, struct proc *fork);

/* arch/ARCH/sys/syscall.c */

/**
 * Sets the result of a thread's system call, in the register that
 * returns it to user mode. That's in the thread's saved registers, or,
 * for a thread that has never run (a child of `fork()`), where it will
 * start from.
 *
 * @param thread  the thread
 * @param val     the result
 */
void arch_syscall_return(struct thread *thread, uintptr_t val);

/* arch/ARCH/sys/sched.c */

/** Starts the timer tick on every CPU, and the clock where it needs to
 * be started. */
void arch_sched_init(void);

/** Ends the current thread. Its kernel stack is freed with it, so this
 * switches to the CPU's idle stack first. It doesn't return. */
void arch_cur_thread_kill(void);

/** Puts the current thread at the end of the ready list, and runs the
 * next ready thread. */
void arch_yield(void);

/* arch/ARCH/sys/execve.c */

/**
 * Sets up a thread to start a new program when it next returns to user
 * mode: at its entry point, with its stack pointer at its initial stack,
 * which the kernel has built already (`argc`, `argv`, `envp` and the
 * auxiliary vector, as the System V ABI has them: `sys/execve.c`). The
 * other registers start as zeros, and so does the FPU's state.
 *
 * @param thread  the thread, which hasn't run since `execve()` (or ever)
 * @param entry   the program's entry point
 * @param sp      the initial stack pointer, 16-byte aligned
 */
void arch_exec_start(struct thread *thread, uintptr_t entry, uintptr_t sp);

/* arch/ARCH/sys/signal.c */

/** Returns from a signal handler, for `sigreturn()`: restores the
 * registers and the signal mask from the handler's ucontext, and the FPU
 * state from its frame. A bad frame gets `SIGSEGV`. */
void arch_sigreturn(void);

/**
 * Returns the user stack pointer of the current thread, as it was when it
 * entered the kernel (for `sigaltstack()`: whether it runs on its
 * alternate stack).
 *
 * @return the stack pointer
 */
uintptr_t arch_user_sp(void);

/**
 * Maps the page with the code that calls `sigreturn()` (the sigcode)
 * into a process's new address space.
 *
 * @param proc  the process
 * @return 0, or a negative errno (`-ENOMEM` if there was no memory)
 */
int  arch_sigcode_map(struct proc *proc);

/* arch/ARCH/mm/ */

/** Finishes setting up memory at boot, once `mm_setup()` has set up the
 * page structures and the buddy allocator: the kernel's page tables. */
void arch_mm_setup(void);

/**
 * Finds the page mapped at a virtual address of an address space.
 *
 * @param pmap   the address space's page tables
 * @param vaddr  the virtual address
 * @return the page's physical address, or 0 if none is mapped there
 */
paddr_t arch_page_get_mapping(struct pmap *pmap, vaddr_t vaddr);

/**
 * Returns a kernel address for a range of physical memory: its address
 * in the direct map if it's there, or a mapping made in a window.
 *
 * @param paddr  the range's physical address
 * @param size   its size, in bytes
 * @return its kernel address, or NULL if it couldn't be mapped
 */
void *arch_phys_map(uintptr_t paddr, size_t size);

/**
 * Maps a device's registers into the kernel's address space, uncached
 * (memory-mapped I/O).
 *
 * @param paddr  the registers' physical address
 * @param size   their size, in bytes
 * @return their kernel address, or NULL if they couldn't be mapped
 */
void *arch_mmio_map(uintptr_t paddr, size_t size);

/* Allocates an IRQ for a device's MSI, with its handler, and returns the
 * message the device must write (address, data). It is weak: an
 * architecture whose interrupt controller takes no MSI doesn't have it. */
int arch_msi_alloc(irq_handler_t handler, void *arg, uint64_t *addr, uint32_t *data) __attribute__((weak));

/** Restarts the machine. */
void arch_reboot(void);

/** Stops the CPU for good, with interrupts off. */
void arch_halt(void) __attribute__((noreturn));

/** Powers the machine off, or halts it if it can't be powered off. */
void arch_poweroff(void) __attribute__((noreturn));

/**
 * Reads the monotonic clock, which starts at boot.
 *
 * @return the time since the clock started, in nanoseconds
 */
uint64_t arch_rtime_ns(void);

/**
 * Reads the monotonic clock, in microseconds.
 *
 * @return the time since the clock started, in microseconds
 */
uint64_t arch_rtime_us(void);

/**
 * Reads the monotonic clock, in milliseconds.
 *
 * @return the time since the clock started, in milliseconds
 */
uint64_t arch_rtime_ms(void);

/**
 * Reads a counter that changes as fast as the CPU runs, which the kernel
 * uses as a source of randomness.
 *
 * @return the counter's value
 */
uint64_t arch_cycles(void);

/**
 * Reads the date and time from the real time clock.
 *
 * @param ts  set to the time since the epoch, to the second (`tv_nsec`
 *            is 0)
 * @return 0
 */
int arch_time_get(struct timespec *ts);

/** Prints the stack trace of the running code, for a panic: the return
 * address of each frame. */
void arch_stack_trace(void);

#endif /* ! _CORE_ARCH_H */
