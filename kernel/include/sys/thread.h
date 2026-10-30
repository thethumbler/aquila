#ifndef _SYS_THREAD_H
#define _SYS_THREAD_H

#include <core/system.h>

struct thread;

#include <sys/proc.h>
#include <sys/waitq.h>
#include <ds/list.h>

/* Threads (sys/thread.c). See docs/kernel/processes.md. */

/* A thread's state. */
typedef enum {
    RUNNABLE,
    ISLEEP, /* sleeping, and a signal can end the sleep */
    USLEEP, /* sleeping, whatever signals come */
    ZOMBIE,
} state_t;

/** A thread. */
struct thread {
    /** the thread's ID, in its process */
    tid_t tid;

    /** its state */
    state_t state;

    /** its process */
    struct proc *owner;

    /** A kernel thread's (process 0's): its name, and what it runs */
    const char *name;
    void (*kentry)(void *arg);
    void *karg;

    /** its user stack */
    uintptr_t stack;
    uintptr_t stack_base;
    size_t    stack_size;

    /** it's on the ready list (sched.c) while it's ready to run, or on
     *  the wait queue it sleeps on (waitq); run_link.list says which */
    struct list_node run_link;
    struct waitq *waitq;

    /** the system call it's in (the last one it made), to restart it */
    int syscall_nr;

    /** whether the call is a restart (a stop interrupted it, and no
     *  handler ran); restart_until is the deadline it had, which a call
     *  that sleeps until a deadline (nanosleep, poll, select) keeps */
    int restarting;
    uint64_t restart_until;

    /** what the thread exited with (thread_exit()), for thread_join() */
    void *exit_value;

    /** whether it's detached (thread_detach()): no one joins it, and it
     *  frees itself when it exits */
    int detached;

    /** the user address of an int set to 0 once it's gone (the C library
     *  then frees its stack), or 0 */
    uintptr_t alive;

    /** its blocked signals, a bit (1 << sig) each: a new thread starts
     *  with its creator's */
    sigset_t sigmask;

    /** the signals pending on it alone (a fault's, pthread_kill()'s),
     *  which it takes before its process's */
    sigset_t sig_pending;

    /** in sigtimedwait(): the signals it waits for, blocked or not; one
     *  sent to the process goes to it first */
    sigset_t sig_waiting;

    /** in sigsuspend() or pselect(): the signal mask they replaced, which
     *  the handler's frame keeps (sigreturn() restores it), or which is
     *  restored on the way out */
    int      sig_oldmask_set;
    sigset_t sig_oldmask;

    /** when it waits with a deadline (sched_wait()): the deadline, in
     *  arch_rtime_ns() time */
    uint64_t sleep_until;

    /** the address of its last fault, the si_addr of the signal it
     *  raised */
    uintptr_t fault_addr;

    /** it's on the timer's list while it waits with a deadline */
    struct list_node timer_link;

    /** the open files its system call holds (file_hold()), let go of
     *  when the call returns, or when the thread is killed */
#define THREAD_FILES_HELD   4
    struct file *files[THREAD_FILES_HELD];
    int files_held;

    /** the architecture's own data: its registers, its kernel stack */
    void *arch;

    /** whether it has run yet */
    int spawned;

    /** it runs before the others once woken, as soon as the interrupt
     *  that woke it returns (sched_thread_wake()): the network's thread */
    int urgent;

    /** in select() or poll(): what it waits for (poll_register()), and
     *  where it sleeps meanwhile */
    struct poll_reg *poll_regs;
    struct waitq poll_wait;

    /** it was killed (proc_threads_end()) while it ran on another CPU: it
     *  frees itself there, when that CPU next enters the kernel; its
     *  owner is NULL if its process goes too (sched_thread_killed()) */
    int killed;
};

/* Making and ending threads (sleeping and waking up are in
 * sys/waitq.h). */
int thread_new(struct proc *proc, struct thread **rthread);
int thread_create(struct thread *thread, uintptr_t stack, uintptr_t entry, uintptr_t uentry, uintptr_t arg, uintptr_t attr, struct thread **new_thread);
int thread_kill(struct thread *thread);
struct thread *thread_find(struct proc *proc, tid_t tid);

/* Lets go of the files a thread's system call holds (fs/file.c). */
void thread_files_release(struct thread *thread);

#endif /* ! _SYS_THREAD_H */
