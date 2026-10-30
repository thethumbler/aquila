/** \file
 * System calls about signals, timers and the time: `kill()` and
 * `pthread_kill()`, `sigaction()`, the signal mask, `sigsuspend()`,
 * `sigtimedwait()` and `sigreturn()`, `setitimer()`, `nanosleep()`, and
 * the clocks.
 */

#include "syscalls.h"

/**
 * Implements `kill()`: sends a signal to a process or a process group.
 *
 * @param pid  whom to: a process if > 0, the caller's process group if 0,
 *             every process but init and the caller if -1, or process
 *             group `-pid` if < -1
 * @param sig  the signal, or 0 to only check that the target exists
 * @return 0, `-EINVAL` for a bad signal, or `-ESRCH` if there is no such
 *         process or group
 */
long sys_kill(pid_t pid, int sig)
{
    syscall_log(LOG_DEBUG, "kill(pid=%d, sig=%d)\n", pid, sig);

    /* a signal sent to ourselves is delivered on the way back to user
     * mode, before kill() returns */
    return signal_send(pid, sig);
}

/**
 * Implements `pthread_kill()`: sends a signal to a thread of the calling
 * process. The thread takes it, unless it blocks it; its action is the
 * process's, so a signal that terminates ends the whole process. Its
 * siginfo says the caller sent it (`SI_USER`).
 *
 * @param tid  the thread's ID
 * @param sig  the signal, or 0 to only check that the thread exists
 * @return 0, `-EINVAL` for a bad signal, or `-ESRCH` if there is no such
 *         thread, or it has exited
 */
long sys_thread_kill(int tid, int sig)
{
    syscall_log(LOG_DEBUG, "thread_kill(tid=%d, sig=%d)\n", tid, sig);

    if (sig < 0 || sig > SIG_MAX)
        return -EINVAL;

    struct thread *thread = thread_find(curproc, tid);

    if (!thread || thread->state == ZOMBIE)
        return -ESRCH;

    struct aquila_siginfo info = {
        .si_signo = sig,
        .si_code  = SI_USER,
        .si_pid   = curproc->pid,
        .si_uid   = curproc->uid,
    };

    return signal_post(curproc, thread, &info);
}

/**
 * Implements `sigaction()`: reads a signal's action, and sets a new one.
 *
 * @param sig   the signal
 * @param act   the new action, or NULL to leave it
 * @param oact  where to put the old action, or NULL
 * @return 0, `-EINVAL` for a bad signal (or catching `SIGKILL` or
 *         `SIGSTOP`), or `-EFAULT`
 */
long sys_sigaction(int sig, const struct aquila_sigaction *act, struct aquila_sigaction *oact)
{
    syscall_log(LOG_DEBUG, "sigaction(sig=%d, act=%p, oact=%p)\n",
            sig, act, oact);

    if (sig < 1 || sig > SIG_MAX) {
        return -EINVAL;
    }

    struct aquila_sigaction new;

    if (act && copyin(&new, act, sizeof(new))) {
        return -EFAULT;
    }

    if (act) {
        uintptr_t handler = new.sa_handler;

        /* SIGKILL and SIGSTOP can't be caught or ignored */
        if ((sig == SIGKILL || sig == SIGSTOP) && handler != SIG_DFL) {
            return -EINVAL;
        }

        /* a handler must be in user memory */
        if (handler != SIG_DFL && handler != SIG_IGN && uaccess_check((void *) handler, 1)) {
            return -EFAULT;
        }
    }

    if (oact && copyout(oact, &curproc->sigaction[sig], sizeof(struct aquila_sigaction))) {
        return -EFAULT;
    }

    if (act) {
        curproc->sigaction[sig] = new;
        signal_action_set(curproc, sig);
    }

    return 0;
}

/**
 * Implements `gettimeofday()`: the time of day.
 *
 * @param tv  where to put the time, or NULL
 * @param tz  where to put the time zone, or NULL
 * @return 0, or `-EFAULT`
 */
long sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    syscall_log(LOG_DEBUG, "gettimeofday(tv=%p, tz=%p)\n", tv, tz);

    struct timeval ktv;
    struct timezone ktz;
    int err = gettimeofday(&ktv, &ktz);

    if (!err && ((tv && copyout(tv, &ktv, sizeof(ktv))) || (tz && copyout(tz, &ktz, sizeof(ktz)))))
        err = -EFAULT;

    return err;
}

/**
 * Implements `sigprocmask()` and `pthread_sigmask()`: reads and changes
 * the calling thread's signal mask. `SIGKILL` and `SIGSTOP` can't be
 * blocked.
 *
 * @param how     `SIG_BLOCK`, `SIG_UNBLOCK` or `SIG_SETMASK`
 * @param set     the signals, or NULL to leave the mask
 * @param oldset  where to put the old mask, or NULL
 * @return 0, `-EINVAL` for a bad `how`, or `-EFAULT`
 */
long sys_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    syscall_log(LOG_DEBUG, "sigmask(how=%d, set=%p, oldset=%p)\n", how, set, oldset);

    sigset_t kset, mask = curthread->sigmask;

    if (set && copyin(&kset, set, sizeof(kset))) {
        return -EFAULT;
    }

    if (set) {
        switch (how) {
            case SIG_BLOCK:   mask |= kset;  break;
            case SIG_UNBLOCK: mask &= ~kset; break;
            case SIG_SETMASK: mask = kset;   break;
            default:
                return -EINVAL;
        }
    }

    if (oldset && copyout(oldset, &curthread->sigmask, sizeof(sigset_t))) {
        return -EFAULT;
    }

    /* a pending signal that is no longer blocked is delivered before this
     * returns to user mode */
    curthread->sigmask = mask & ~SIG_UNMASKABLE;
    return 0;
}

/**
 * Implements `sigpending()`: the signals that are pending for the calling
 * thread, its own and its process's, and that it blocks.
 *
 * @param set  where to put them
 * @return 0, or `-EFAULT`
 */
long sys_sigpending(sigset_t *set)
{
    syscall_log(LOG_DEBUG, "sigpending(set=%p)\n", set);

    sigset_t pending = (curthread->sig_pending | curproc->sig_pending) & curthread->sigmask;

    return copyout(set, &pending, sizeof(pending));
}

/**
 * Implements `sigsuspend()`: replaces the calling thread's signal mask and
 * waits for a signal. The old mask comes back once a handler has run.
 *
 * @param set  the mask to wait with
 * @return `-EINTR` once a handler has run (it is restarted after a stop),
 *         or `-EFAULT`
 */
long sys_sigsuspend(const sigset_t *set)
{
    syscall_log(LOG_DEBUG, "sigsuspend(set=%p)\n", set);

    sigset_t kset;

    if (copyin(&kset, set, sizeof(kset))) {
        return -EFAULT;
    }

    struct thread *thread = curthread;
    struct waitq wait = {0};

    /* the mask it replaces goes in the handler's frame (sigreturn()
     * restores it), or comes back if the call is restarted */
    thread->sig_oldmask = thread->sigmask;
    thread->sig_oldmask_set = 1;
    thread->sigmask = kset & ~SIG_UNMASKABLE;

    /* sleep until a signal it doesn't block comes (nothing else wakes it
     * up) */
    while (!waitq_sleep(&wait))
        ;

    /* EINTR once a handler ran; restarted if none did (a stop) */
    return -ERESTARTNOHAND;
}

/**
 * Implements `sigtimedwait()` (and `sigwait()`, `sigwaitinfo()`): waits
 * for a signal of a set, which the caller blocks, and takes it without
 * running its handler. A stop doesn't change the deadline: the call is
 * restarted until the same time.
 *
 * @param set      the signals to wait for (`SIGKILL` and `SIGSTOP` aren't
 *                 waited for)
 * @param info     where to put the signal's siginfo, or NULL
 * @param timeout  how long to wait at most, or NULL for no limit
 * @return the signal, or a negative errno: `-EAGAIN` once the time is up,
 *         `-EINTR` if another signal's handler ran, `-EINVAL` for a bad
 *         time, `-EFAULT`
 */
long sys_sigtimedwait(const sigset_t *set, struct aquila_siginfo *info,
                      const struct aquila_timespec *timeout)
{
    syscall_log(LOG_DEBUG, "sigtimedwait(set=%p, info=%p, timeout=%p)\n", set, info, timeout);

    sigset_t kset;
    struct aquila_timespec kts;

    if (copyin(&kset, set, sizeof(kset)) || (timeout && copyin(&kts, timeout, sizeof(kts)))) {
        return -EFAULT;
    }

    if (timeout && ((int64_t) kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000)) {
        return -EINVAL;
    }

    kset &= ~SIG_UNMASKABLE;

    /* a call restarted after a stop keeps the deadline it had; with no
     * timeout there's none (0), and a zero timeout only looks */
    struct thread *thread = curthread;
    uint64_t until = 0;

    if (timeout) {
        until = thread->restarting ? thread->restart_until :
                arch_rtime_ns() + kts.tv_sec * 1000000000ULL + kts.tv_nsec;
        thread->restart_until = until;
    }

    struct waitq wait = {0};
    struct aquila_siginfo kinfo;
    int sig;

    for (;;) {
        if ((sig = signal_dequeue(thread, kset, &kinfo)))
            break;

        if (timeout && arch_rtime_ns() >= until) {
            return -EAGAIN;
        }

        /* nothing but a signal wakes it up, or the deadline */
        thread->sig_waiting = kset;
        int r = sched_wait(&wait, until);
        thread->sig_waiting = 0;

        /* another signal, one it doesn't wait for: EINTR once its handler
         * ran, a restart after a stop */
        if (r < 0 && !(sig = signal_dequeue(thread, kset, &kinfo))) {
            return -ERESTARTNOHAND;
        }

        if (sig)
            break;
    }

    if (info && copyout(info, &kinfo, sizeof(kinfo))) {
        return -EFAULT;
    }

    return sig;
}

/**
 * Implements `sigaltstack()`: reads and sets the calling thread's
 * alternate signal stack, where the handlers with `SA_ONSTACK` run. It
 * can't be changed while the thread runs on it.
 *
 * @param ss   the new stack, or NULL to leave it: `SS_DISABLE` for none
 * @param oss  where to put the old one, or NULL
 * @return 0, or a negative errno: `-EPERM` (running on it), `-EINVAL` (bad
 *         flags), `-ENOMEM` (smaller than `MINSIGSTKSZ`), `-EFAULT`
 */
long sys_sigaltstack(const struct aquila_stack *ss, struct aquila_stack *oss)
{
    syscall_log(LOG_DEBUG, "sigaltstack(ss=%p, oss=%p)\n", ss, oss);

    struct thread *thread = curthread;
    struct aquila_stack new, old;

    if (ss && copyin(&new, ss, sizeof(new))) {
        return -EFAULT;
    }

    signal_altstack_get(thread, arch_user_sp(), &old);

    if (ss) {
        if (old.ss_flags & SS_ONSTACK) {
            return -EPERM;
        }

        if (new.ss_flags & ~SS_DISABLE) {
            return -EINVAL;
        }

        if (!(new.ss_flags & SS_DISABLE) && new.ss_size < MINSIGSTKSZ) {
            return -ENOMEM;
        }
    }

    if (oss && copyout(oss, &old, sizeof(old))) {
        return -EFAULT;
    }

    if (ss) {
        /* none: a size of 0; a stack past user space would only make
         * the frame's copyout() fail, as one that isn't mapped */
        if (new.ss_flags & SS_DISABLE)
            new.ss_sp = new.ss_size = 0;

        new.ss_flags = 0;
        thread->sig_altstack = new;
    }

    return 0;
}

/**
 * Implements `sigreturn()`: a signal handler returns, and the registers
 * and signal mask that its frame saved are restored. The kernel's signal
 * code page calls it when a handler returns.
 *
 * @return 0, which `syscall_dispatch()` ignores: the registers are the
 *         frame's
 */
long sys_sigreturn(void)
{
    syscall_log(LOG_DEBUG, "sigreturn()\n");
    arch_sigreturn();

    /* the registers are the frame's now: syscall_dispatch() doesn't set
     * a result */
    return 0;
}

/**
 * Implements `setitimer()`: sets the real time timer, which sends
 * `SIGALRM`, and reads the old one.
 *
 * @param which  `AQUILA_ITIMER_REAL` (the only timer supported)
 * @param new    the new timer, or NULL to only read the old one
 * @param old    where to put the old timer, or NULL
 * @return 0, `-EINVAL` for another timer, or `-EFAULT`
 */
long sys_setitimer(int which, const struct aquila_itimer *new, struct aquila_itimer *old)
{
    syscall_log(LOG_DEBUG, "setitimer(which=%d, new=%p, old=%p)\n", which, new, old);

    struct aquila_itimer knew;

    if (new && copyin(&knew, new, sizeof(knew))) {
        return -EFAULT;
    }

    /* only the real time timer: the kernel doesn't count virtual time */
    if (which != AQUILA_ITIMER_REAL) {
        return -EINVAL;
    }

    struct aquila_itimer prev = {0};

    /* check that the old timer can be written out before setting the new
     * one */
    if (old && copyout(old, &prev, sizeof(prev))) {
        return -EFAULT;
    }

    if (new)
        sched_alarm(curproc, knew.value, knew.interval, &prev.value, &prev.interval);
    else
        sched_alarm_get(curproc, &prev.value, &prev.interval);

    return old ? copyout(old, &prev, sizeof(prev)) : 0;
}

/** Converts a time from user memory to ns, for the sleeps. Returns 0, or
 * `-EINVAL` if it isn't a time (negative, or nanoseconds of a second or
 * more). A time of more than 2^33 seconds (272 years) is cut there, so
 * that adding it to now can't wrap around. */
static int timespec_ns(const struct aquila_timespec *ts, uint64_t *ns)
{
    if ((int64_t) ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
        return -EINVAL;

    *ns = MIN(ts->tv_sec, 1ULL << 33) * 1000000000ULL + ts->tv_nsec;
    return 0;
}

/** Sleeps until `until` (`arch_rtime_ns()` time), for `nanosleep()` and
 * `clock_nanosleep()`. If a signal ends the sleep, the time left is
 * written to `rem` (unless it's NULL), and the call fails with `EINTR`
 * once a handler ran, or is restarted after a stop (until the same time:
 * the caller takes `restart_until` then). Returns 0 or a negative errno. */
static long sleep_until(uint64_t until, struct aquila_timespec *rem)
{
    curthread->restart_until = until;

    if (!sched_sleep(until))
        return 0;

    if (rem) {
        uint64_t now = arch_rtime_ns();
        uint64_t left = now < until ? until - now : 0;
        struct aquila_timespec krem = {
            .tv_sec  = left / 1000000000ULL,
            .tv_nsec = left % 1000000000ULL,
        };

        if (copyout(rem, &krem, sizeof(krem))) {
            return -EFAULT;
        }
    }

    return -ERESTARTNOHAND;
}

/**
 * Implements `nanosleep()`: sleeps for a time. A stop doesn't change the
 * deadline: the call is restarted until the same time.
 *
 * @param req  how long to sleep
 * @param rem  where to put the time left if a signal ends the sleep, or
 *             NULL
 * @return 0, `-EINTR` if a handler ran, `-EINVAL` for a bad time, or
 *         `-EFAULT`
 */
long sys_nanosleep(const struct aquila_timespec *req, struct aquila_timespec *rem)
{
    syscall_log(LOG_DEBUG, "nanosleep(req=%p, rem=%p)\n", req, rem);

    struct aquila_timespec kreq;
    uint64_t ns;
    int err;

    if (copyin(&kreq, req, sizeof(kreq))) {
        return -EFAULT;
    }

    if ((err = timespec_ns(&kreq, &ns))) {
        return err;
    }

    /* a call restarted after a stop keeps the deadline it had; otherwise
     * the deadline is counted from now */
    uint64_t until = curthread->restarting ? curthread->restart_until :
                     arch_rtime_ns() + ns;

    return sleep_until(until, rem);
}

/**
 * Implements `clock_nanosleep()`: `nanosleep()` on a clock, for a time or
 * until a time (`AQUILA_TIMER_ABSTIME`). A time already past doesn't
 * sleep. The C library returns the error itself, rather than setting
 * `errno`, as POSIX says.
 *
 * @param clock  `CLOCK_REALTIME` or `CLOCK_MONOTONIC`
 * @param flags  `AQUILA_TIMER_ABSTIME` if `req` is when to wake up
 * @param req    how long to sleep, or until when
 * @param rem    where to put the time left if a signal ends a relative
 *               sleep, or NULL; an absolute sleep doesn't write it
 * @return 0, `-EINTR` if a handler ran, `-EINVAL` for a bad clock, flag
 *         or time, or `-EFAULT`
 */
long sys_clock_nanosleep(int clock, int flags, const struct aquila_timespec *req,
                         struct aquila_timespec *rem)
{
    syscall_log(LOG_DEBUG, "clock_nanosleep(clock=%d, flags=%x, req=%p, rem=%p)\n",
            clock, flags, req, rem);

    struct aquila_timespec kreq;
    uint64_t ns;
    int err;

    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        return -EINVAL;
    }

    if (flags & ~AQUILA_TIMER_ABSTIME) {
        return -EINVAL;
    }

    if (copyin(&kreq, req, sizeof(kreq))) {
        return -EFAULT;
    }

    if ((err = timespec_ns(&kreq, &ns))) {
        return err;
    }

    uint64_t now = arch_rtime_ns();
    uint64_t until;

    if (curthread->restarting) {
        until = curthread->restart_until;
    } else if (!(flags & AQUILA_TIMER_ABSTIME)) {
        until = now + ns;
    } else if (clock == CLOCK_MONOTONIC) {
        /* the monotonic clock is arch_rtime_ns() */
        until = ns;
    } else {
        /* the real time clock: the time from now until then, from now on
         * the monotonic clock (setting the clock meanwhile, which can't
         * be done yet, wouldn't change the deadline) */
        struct timespec ts;
        gettime(&ts);

        uint64_t real = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        until = now + (ns > real ? ns - real : 0);
    }

    return sleep_until(until, flags & AQUILA_TIMER_ABSTIME ? NULL : rem);
}

/**
 * Implements `clock_gettime()`: reads a clock.
 *
 * @param clock  `CLOCK_REALTIME` or `CLOCK_MONOTONIC`
 * @param ts     where to put the time
 * @return 0, `-EINVAL` for another clock, or `-EFAULT`
 */
long sys_clock_gettime(int clock, struct aquila_timespec *ts)
{
    syscall_log(LOG_DEBUG, "clock_gettime(clock=%d, ts=%p)\n", clock, ts);

    struct aquila_timespec kts;
    struct timespec now;
    uint64_t ns;

    switch (clock) {
        case CLOCK_REALTIME:
            gettime(&now);
            kts.tv_sec  = now.tv_sec;
            kts.tv_nsec = now.tv_nsec;
            break;
        case CLOCK_MONOTONIC:
            ns = arch_rtime_ns();
            kts.tv_sec  = ns / 1000000000ULL;
            kts.tv_nsec = ns % 1000000000ULL;
            break;
        default:
            return -EINVAL;
    }

    return copyout(ts, &kts, sizeof(kts));
}
