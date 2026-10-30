/** \file
 * Signals: sending them, and taking them on the way back to user mode.
 *
 * Sending a signal makes it pending: on the process (`proc->sig_pending`,
 * for `kill()`), or on one thread (`thread->sig_pending`, for a fault or
 * `pthread_kill()`). Each thread has its own mask. A process's signal
 * wakes up one thread that doesn't block it: if that thread sleeps where
 * a signal may end the sleep (`waitq_sleep()`), the sleep returns
 * `-ERESTARTSYS`, and its system call unwinds. A signal is only delivered
 * on the way back to user mode: the architecture's return path calls
 * `signal_take()`, which carries out stops and default actions, and runs
 * the handler it returns. So the kernel never leaves anything halfway for
 * a signal. See docs/kernel/signals.md.
 */

#include <core/system.h>
#include <core/arch.h>

#include <ds/queue.h>

#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/kthread.h>
#include <sys/sched.h>

#include <aquila/errno.h>

/* What each signal does when it isn't caught (its default action). */
int sig_default_action[SIG_MAX+1] = {
    [SIGABRT] = SIGACT_ABORT,
    [SIGALRM] = SIGACT_TERMINATE,
    [SIGBUS]  = SIGACT_ABORT,
    [SIGCHLD] = SIGACT_IGNORE,
    [SIGCONT] = SIGACT_CONTINUE,
    [SIGEMT]  = SIGACT_ABORT,
    [SIGFPE]  = SIGACT_ABORT,
    [SIGHUP]  = SIGACT_TERMINATE,
    [SIGILL]  = SIGACT_ABORT,
    [SIGINT]  = SIGACT_TERMINATE,
    [SIGKILL] = SIGACT_TERMINATE,
    [SIGPIPE] = SIGACT_TERMINATE,
    [SIGQUIT] = SIGACT_ABORT,
    [SIGSEGV] = SIGACT_ABORT,
    [SIGSTOP] = SIGACT_STOP,
    [SIGTERM] = SIGACT_TERMINATE,
    [SIGTSTP] = SIGACT_STOP,
    [SIGTTIN] = SIGACT_STOP,
    [SIGTTOU] = SIGACT_STOP,
    [SIGUSR1] = SIGACT_TERMINATE,
    [SIGUSR2] = SIGACT_TERMINATE,
    [SIGPOLL] = SIGACT_TERMINATE,
    [SIGPROF] = SIGACT_TERMINATE,
    [SIGSYS]  = SIGACT_ABORT,
    [SIGTRAP] = SIGACT_ABORT,
    [SIGURG]  = SIGACT_IGNORE,
    [SIGWINCH] = SIGACT_IGNORE,
    [SIGVTALRM] = SIGACT_TERMINATE,
    [SIGXCPU] = SIGACT_ABORT,
    [SIGXFSZ] = SIGACT_ABORT,
    [SIGLOST] = SIGACT_TERMINATE,
};

/**
 * Says whether delivering a signal to a process would stop it: the signal
 * isn't caught, and its default action is to stop.
 *
 * @param proc  the process
 * @param sig   the signal
 * @return 1 if it would, 0 if not
 */
int signal_stops(struct proc *proc, int sig)
{
    return proc->sigaction[sig].sa_handler == SIG_DFL &&
           sig_default_action[sig] == SIGACT_STOP;
}

/** Tells a process's parent that it stopped or continued (`code` is
 * `CLD_STOPPED` or `CLD_CONTINUED`, `sig` the signal that did it): its
 * `wait()` wakes up, and it gets `SIGCHLD` unless it asked not to
 * (`SA_NOCLDSTOP`). */
static void signal_notify_parent(struct proc *proc, int code, int sig)
{
    struct proc *parent = proc->parent;

    if (!parent)
        return;

    waitq_wakeup(&parent->wait_queue);

    if (!(parent->sigaction[SIGCHLD].sa_flags & SA_NOCLDSTOP)) {
        struct aquila_siginfo info = {
            .si_signo  = SIGCHLD,
            .si_code   = code,
            .si_pid    = proc->pid,
            .si_uid    = proc->uid,
            .si_status = sig,
        };

        signal_post(parent, NULL, &info);
    }
}

/**
 * Stops the current process, until `SIGCONT` or `SIGKILL`. Its thread
 * calls this on its way back to user mode (`signal_take()`), and sleeps
 * here; other signals don't end the sleep, only continuing (or killing)
 * the process does.
 *
 * @param sig  the signal that stops it
 */
void signal_stop(int sig)
{
    struct proc *proc = curproc;

    proc->stopped = sig;
    proc->wait_event = (proc->wait_event & ~WAIT_CONTINUED) | WAIT_STOPPED;
    signal_notify_parent(proc, CLD_STOPPED, sig);

    /* the whole process stops: its other threads stop on their way back
     * to user mode (signal_take()); those running on other CPUs go there
     * now, and the sleeping ones when they wake up */
    queue_for (node, &proc->threads) {
        struct thread *thread = node->value;

        if (thread != curthread)
            sched_thread_kick(thread);
    }

    while (proc->stopped)
        waitq_sleep_unint(&proc->stop_queue);
}

#define STOP_SIGNALS    ((1UL << SIGSTOP) | (1UL << SIGTSTP) | (1UL << SIGTTIN) | (1UL << SIGTTOU))

/** Returns whether `sig` is one of the stop signals. */
static int is_stop_signal(int sig)
{
    return !!(STOP_SIGNALS & (1UL << sig));
}

/** Returns whether delivering `sig` would do nothing: the process ignores
 * it, or its default action is to ignore it. `SIGCONT` counts, since its
 * action is done when it is sent. */
static int signal_ignored(struct proc *proc, int sig)
{
    uintptr_t handler = proc->sigaction[sig].sa_handler;

    if (handler == SIG_IGN)
        return 1;

    return handler == SIG_DFL && (sig_default_action[sig] == SIGACT_IGNORE ||
                                  sig_default_action[sig] == SIGACT_CONTINUE);
}

/** Removes signals from what is pending on a process and its threads. */
static void signal_discard(struct proc *proc, sigset_t set)
{
    proc->sig_pending &= ~set;

    queue_for (node, &proc->threads)
        ((struct thread *) node->value)->sig_pending &= ~set;
}

/** Makes a thread take its signals soon: wakes it up if it sleeps where a
 * signal may end the sleep, or interrupts it if it runs on another CPU. */
static void signal_wake(struct thread *thread)
{
    if (thread->state == ISLEEP && thread->waitq)
        thread_wakeup(thread);
    else
        sched_thread_kick(thread);
}

/** Says whether a thread would take `sig` now: it's alive, and doesn't
 * block it or waits for it (sigtimedwait()). */
static int signal_takes(struct thread *thread, int sig)
{
    sigset_t bit = 1UL << sig;

    return thread->state != ZOMBIE && !thread->killed &&
           (!(thread->sigmask & bit) || (thread->sig_waiting & bit));
}

/**
 * Sends a signal to a process, or to one of its threads: makes it pending,
 * with its siginfo (unless it is already: the first sending's is kept),
 * and wakes up a thread that would take it on its way back to user mode.
 * A process's signal goes to a thread waiting for it in `sigtimedwait()`
 * first, else to the first one that doesn't block it; if they all do, it
 * stays pending until one unblocks it. Returns 0, `-EINVAL` for a bad
 * signal, or `-EPERM` for process 0.
 */
static int signal_to(struct proc *proc, struct thread *target, const struct aquila_siginfo *info)
{
    int signal = info->si_signo;

    if (signal < 0 || signal > SIG_MAX)
        return -EINVAL;

    /* the kernel's threads (process 0) take no signals */
    if (proc == kproc)
        return -EPERM;

    /* signal 0 only checks that the process exists, and zombies can't be
     * signalled */
    if (!signal || !proc->running)
        return 0;

    /* a stop signal cancels a pending SIGCONT and the other way around;
     * SIGCONT (whatever its action) and SIGKILL resume a stopped process */
    if (is_stop_signal(signal)) {
        signal_discard(proc, 1UL << SIGCONT);
    } else if (signal == SIGCONT || signal == SIGKILL) {
        signal_discard(proc, STOP_SIGNALS);

        if (proc->stopped) {
            proc->stopped = 0;

            if (signal == SIGCONT) {
                proc->wait_event = (proc->wait_event & ~WAIT_STOPPED) | WAIT_CONTINUED;
                signal_notify_parent(proc, CLD_CONTINUED, SIGCONT);
            }

            waitq_wakeup(&proc->stop_queue);
        }
    }

    /* a signal that would be ignored is discarded when it's sent, blocked
     * or not (POSIX allows it) */
    if (signal_ignored(proc, signal))
        return 0;

    /* a thread's own signal: it takes it, unless it blocks it */
    if (target) {
        if (target->state == ZOMBIE || target->killed)
            return 0;

        if (!(target->sig_pending & (1UL << signal)))
            target->sig_info[signal] = *info;

        target->sig_pending |= 1UL << signal;

        if (signal_takes(target, signal))
            signal_wake(target);

        return 0;
    }

    if (!(proc->sig_pending & (1UL << signal)))
        proc->sig_info[signal] = *info;

    proc->sig_pending |= 1UL << signal;

    /* SIGKILL ends every thread: they all go */
    if (signal == SIGKILL) {
        queue_for (node, &proc->threads)
            signal_wake(node->value);

        return 0;
    }

    /* one thread takes it: one waiting for it, else one not blocking it */
    struct thread *taker = NULL;

    queue_for (node, &proc->threads) {
        struct thread *thread = node->value;

        if (signal_takes(thread, signal) && (thread->sig_waiting & (1UL << signal))) {
            taker = thread;
            break;
        }

        if (!taker && signal_takes(thread, signal))
            taker = thread;
    }

    if (taker)
        signal_wake(taker);

    return 0;
}

/**
 * Discards a pending signal that `sigaction()` just made ignored.
 *
 * @param proc  the process
 * @param sig   the signal whose action changed
 */
void signal_action_set(struct proc *proc, int sig)
{
    if (signal_ignored(proc, sig))
        signal_discard(proc, 1UL << sig);
}

/**
 * Says whether a thread has a signal to take, which ends a sleep that a
 * signal may end.
 *
 * @param thread  the thread
 * @return 1 if it or its process has a pending signal that it doesn't
 *         block, or that it waits for (`sigtimedwait()`), 0 if not
 */
int signal_interrupts(struct thread *thread)
{
    struct proc *proc = thread->owner;

    if (!proc || proc == kproc)
        return 0;

    sigset_t pending = thread->sig_pending | proc->sig_pending;

    return !!(pending & (~thread->sigmask | thread->sig_waiting));
}

/** Takes signal `sig` off what is pending for a thread, its own else its
 * process's, and puts its siginfo in `info`. */
static void signal_unpend(struct thread *thread, int sig, struct aquila_siginfo *info)
{
    struct proc *proc = thread->owner;

    if (thread->sig_pending & (1UL << sig)) {
        thread->sig_pending &= ~(1UL << sig);
        *info = thread->sig_info[sig];
    } else {
        proc->sig_pending &= ~(1UL << sig);
        *info = proc->sig_info[sig];
    }
}

/**
 * Takes a pending signal of a set for a thread, blocked or not, for
 * `sigtimedwait()`.
 *
 * @param thread  the thread
 * @param set     the signals it waits for
 * @param info    where to put the signal's siginfo
 * @return the lowest numbered one pending, the thread's own before its
 *         process's, or 0 if none is
 */
int signal_dequeue(struct thread *thread, sigset_t set, struct aquila_siginfo *info)
{
    sigset_t ready = thread->sig_pending & set;

    if (!ready)
        ready = thread->owner->sig_pending & set;

    if (!ready)
        return 0;

    int sig = __builtin_ctzl(ready);
    signal_unpend(thread, sig, info);
    return sig;
}

/**
 * Sends a thread a signal that can't be blocked or ignored, such as the
 * `SIGSEGV` of its fault: if the thread blocks it or the process ignores
 * it, it is unblocked and its action reset to the default. Its siginfo is
 * this fault's, even if the signal was pending already.
 *
 * @param thread  the thread
 * @param signal  the signal
 * @param code    its `si_code` (`SEGV_MAPERR`...), or `SI_KERNEL`
 * @param addr    its `si_addr`: the address, or the instruction
 */
void signal_force(struct thread *thread, int signal, int code, uintptr_t addr)
{
    struct proc *proc = thread->owner;
    struct aquila_siginfo info = {
        .si_signo = signal,
        .si_code  = code,
        .si_addr  = addr,
    };

    if ((thread->sigmask & (1UL << signal)) || proc->sigaction[signal].sa_handler == SIG_IGN) {
        thread->sigmask &= ~(1UL << signal);
        proc->sigaction[signal].sa_handler = SIG_DFL;
    }

    thread->sig_pending &= ~(1UL << signal);
    signal_to(proc, thread, &info);
}

/**
 * Ends the current process because of a signal. It doesn't return.
 *
 * @param sig  the signal, which its parent's `wait()` reports
 */
void signal_exit(int sig)
{
    curproc->exit = PROC_EXIT(sig, sig);
    proc_kill(curproc);
    arch_sleep();

    for (;;);
}

/**
 * Takes the current thread's pending signals, its own and its process's,
 * on the way back to user mode: ignored ones are dropped, and default
 * actions are carried out (ending or stopping the process), until one
 * with a handler is found. A thread of a stopped process stops here
 * until it is continued.
 *
 * @param info  where to put the siginfo of the signal returned
 * @return a signal whose handler the architecture runs now, or 0 if there
 *         is none
 */
int signal_take(struct aquila_siginfo *info)
{
    struct thread *thread = curthread;
    struct proc *proc = curproc;

    for (;;) {
        /* another thread stopped the process */
        while (proc->running && proc->stopped)
            waitq_sleep_unint(&proc->stop_queue);

        sigset_t ready = (thread->sig_pending | proc->sig_pending) & ~thread->sigmask;

        if (!proc->running || !ready)
            return 0;

        /* SIGKILL first, then the lowest numbered signal */
        int sig = (ready & (1UL << SIGKILL)) ? SIGKILL : __builtin_ctzl(ready);
        signal_unpend(thread, sig, info);

        uintptr_t handler = proc->sigaction[sig].sa_handler;

        if (handler == SIG_IGN)
            continue;

        if (handler != SIG_DFL)
            return sig;

        switch (sig_default_action[sig]) {
            case SIGACT_ABORT:
            case SIGACT_TERMINATE:
                signal_exit(sig);
            case SIGACT_STOP:
                /* the terminal's stop signals don't stop an orphaned
                 * process group, since no one would continue it (POSIX);
                 * SIGSTOP does */
                if (sig != SIGSTOP && pgrp_orphaned(proc->pgrp, NULL))
                    break;

                signal_stop(sig);
                break;
            default:
                break;
        }
    }
}

/** Returns whether the stack pointer `sp` is on the thread's alternate
 * stack. */
static int signal_on_altstack(struct thread *thread, uintptr_t sp)
{
    struct aquila_stack *ss = &thread->sig_altstack;

    return ss->ss_size && sp > ss->ss_sp && sp - ss->ss_sp <= ss->ss_size;
}

/**
 * Says where a handler's frame goes: on the thread's alternate stack if
 * the action has `SA_ONSTACK` and the thread has one, and isn't on it
 * already (a handler interrupted there goes on below where it was).
 *
 * @param thread  the thread
 * @param sa      the signal's action
 * @param sp      the user stack pointer the signal interrupted
 * @return the top of the alternate stack, or 0 if the frame goes on the
 *         stack the thread is on
 */
uintptr_t signal_altstack_top(struct thread *thread, struct aquila_sigaction *sa, uintptr_t sp)
{
    struct aquila_stack *ss = &thread->sig_altstack;

    if (!(sa->sa_flags & SA_ONSTACK) || !ss->ss_size || signal_on_altstack(thread, sp))
        return 0;

    return ss->ss_sp + ss->ss_size;
}

/**
 * Describes a thread's alternate stack, as `sigaltstack()` reports it:
 * where it is, and whether the thread runs on it (`SS_ONSTACK`), or that
 * there is none (`SS_DISABLE`).
 *
 * @param thread  the thread
 * @param sp      its user stack pointer
 * @param ss      where to put it
 */
void signal_altstack_get(struct thread *thread, uintptr_t sp, struct aquila_stack *ss)
{
    *ss = thread->sig_altstack;

    if (!ss->ss_size)
        ss->ss_flags = SS_DISABLE;
    else
        ss->ss_flags = signal_on_altstack(thread, sp) ? SS_ONSTACK : 0;
}

/**
 * Sends a signal with its siginfo, to a process or to one of its threads.
 *
 * @param proc    the process
 * @param thread  the thread, or NULL for any thread of the process
 * @param info    the signal (`si_signo`, 0 only checks that the process
 *                exists) and its siginfo
 * @return 0, `-EINVAL` for a bad signal, or `-EPERM` for process 0
 */
int signal_post(struct proc *proc, struct thread *thread, const struct aquila_siginfo *info)
{
    return signal_to(proc, thread, info);
}

/** Makes the siginfo of a signal the kernel sends (`SI_KERNEL`). */
#define KERNEL_INFO(sig)    ((struct aquila_siginfo) {.si_signo = (sig), .si_code = SI_KERNEL})

/**
 * Sends a signal of the kernel's (a terminal's, a timer's...) to a
 * process.
 *
 * @param proc    the process
 * @param signal  the signal (0 only checks that the process exists)
 * @return 0, `-EINVAL` for a bad signal, or `-EPERM` for process 0
 */
int signal_proc_send(struct proc *proc, int signal)
{
    struct aquila_siginfo info = KERNEL_INFO(signal);
    return signal_to(proc, NULL, &info);
}

/**
 * Sends a signal to one thread (for `pthread_kill()`, or a signal that its
 * own action raised, such as `SIGPIPE`): that thread takes it, once it
 * doesn't block it.
 *
 * @param thread  the thread
 * @param signal  the signal (0 only checks that the thread exists)
 * @return 0, `-EINVAL` for a bad signal, or `-EPERM` for process 0
 */
int signal_thread_send(struct thread *thread, int signal)
{
    struct aquila_siginfo info = KERNEL_INFO(signal);
    return signal_to(thread->owner, thread, &info);
}

/**
 * Sends a signal to every process of a process group.
 *
 * @param pg      the process group
 * @param signal  the signal
 * @return 0
 */
int signal_pgrp_send(struct pgroup *pg, int signal)
{
    struct aquila_siginfo info = KERNEL_INFO(signal);

    queue_for (node, pg->procs)
        signal_to(node->value, NULL, &info);

    return 0;
}

/**
 * Sends a signal as `kill()` does: its siginfo says the calling process
 * sent it (`SI_USER`, its pid and user id).
 *
 * @param pid     whom to: a process if `pid` > 0, the caller's process
 *                group if 0, every process but init and the caller if -1,
 *                or process group `-pid` if < -1
 * @param signal  the signal
 * @return 0, `-EINVAL` for a bad signal, or `-ESRCH` if no such process
 *         or group exists
 */
int signal_send(pid_t pid, int signal)
{
    if (signal < 0 || signal > SIG_MAX)
        return -EINVAL;

    /* who sent it */
    struct aquila_siginfo info = {
        .si_signo = signal,
        .si_code  = SI_USER,
        .si_pid   = curproc->pid,
        .si_uid   = curproc->uid,
    };

    if (pid > 0) {
        struct proc *proc = proc_pid_find(pid);

        if (!proc)
            return -ESRCH;

        return signal_to(proc, NULL, &info);
    }

    if (pid == -1) {
        int found = 0;

        queue_for (node, procs) {
            struct proc *proc = node->value;

            if (proc->pid == 1 || proc == curproc || proc == kproc)
                continue;

            signal_to(proc, NULL, &info);
            found = 1;
        }

        return found ? 0 : -ESRCH;
    }

    pid_t pgid = pid ? -pid : curproc->pgrp->pgid;
    int found = 0;

    queue_for (node, procs) {
        struct proc *proc = node->value;

        if (!proc->running || proc->pgrp->pgid != pgid)
            continue;

        found = 1;
        signal_to(proc, NULL, &info);
    }

    return found ? 0 : -ESRCH;
}
