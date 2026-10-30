#ifndef _SYS_SIGNAL_H
#define _SYS_SIGNAL_H

#include <core/system.h>

struct aquila_sigaction;

#include <sys/proc.h>

/* Signals (sys/signal.c). See docs/kernel/signals.md. */

/* the signals' numbers */
#include <aquila/signal.h>

/* The highest signal number. */
#define SIG_MAX (NSIG - 1)

/* A signal's default action. */
#define SIGACT_ABORT        1
#define SIGACT_TERMINATE    2
#define SIGACT_IGNORE       3
#define SIGACT_STOP         4
#define SIGACT_CONTINUE     5

/* A handler's special values, as the C library's. */
#define SIG_DFL ((uintptr_t) 0)  /* the default action */
#define SIG_IGN ((uintptr_t) 1)  /* ignore the signal */
#define SIG_ERR ((uintptr_t) -1) /* an error */

/* Each signal's default action (SIGACT_*). */
extern int sig_default_action[SIG_MAX+1];

/*
 * Sending a signal makes it pending (a bit for each signal), on the
 * process or on one thread, and wakes up a thread that doesn't block it.
 * Signals are delivered only on the way back to user mode (signal_take(),
 * from the architecture's return path), never from the middle of the
 * kernel: a sleep that a signal interrupts returns ERESTARTSYS, and the
 * system call unwinds.
 */
int signal_send(int pid, int sig);
int signal_proc_send(struct proc *proc, int signal);
int signal_thread_send(struct thread *thread, int signal);
int signal_pgrp_send(struct pgroup *pg, int signal);

/* Notes that sigaction() changed a signal's action (a pending signal that
 * is now ignored is discarded). */
void signal_action_set(struct proc *proc, int sig);

/* Sends a fault's signal (SIGSEGV, SIGILL...): it is delivered even if
 * it's blocked or ignored, by its default action then, because the
 * instruction would only fault again. */
void signal_force(struct thread *thread, int signal);

/* Says whether a signal is pending that the thread doesn't block (or
 * waits for, in sigtimedwait()), which ends its interruptible sleeps. */
int signal_interrupts(struct thread *thread);

/* Takes a pending signal of `set` for the thread, blocked or not (for
 * sigtimedwait()): the lowest numbered, its own before its process's.
 * Returns it, or 0 if none is pending. */
int signal_dequeue(struct thread *thread, sigset_t set);

/* On the way back to user mode: stops the process, or does the default
 * actions, of the pending signals (terminating doesn't return), and
 * returns the signal whose handler is to run, or 0. */
int signal_take(void);

/* Ends the current process, by `sig`, from where it can't go on. */
void signal_exit(int sig) __attribute__((noreturn));

/* Stopping and continuing (job control): what waitpid() reports, whether
 * a signal stops a process, and stopping the current one. */
#define WAIT_STOPPED    1
#define WAIT_CONTINUED  2

int  signal_stops(struct proc *proc, int sig);
void signal_stop(int sig);

/*
 * What a system call that a signal interrupted returns (a sleep returned
 * it). With ERESTARTSYS, the call is restarted on the way out if no
 * handler runs, or if the handler has SA_RESTART. With ERESTARTNOHAND,
 * it's restarted only if no handler runs: for the waits that POSIX says
 * fail with EINTR whatever SA_RESTART says. User space never sees these
 * values: it gets EINTR.
 */
#define ERESTARTSYS     512
#define ERESTARTNOHAND  514

/* The signals that can't be blocked. */
#define SIG_UNMASKABLE  ((1UL << SIGKILL) | (1UL << SIGSTOP))

#endif /* ! _SYS_SIGNAL_H */
