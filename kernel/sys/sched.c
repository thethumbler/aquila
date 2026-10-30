/** \file
 * The scheduler: which thread runs next, and the tick.
 *
 * It is a round robin. The threads that are ready to run wait on one
 * list, shared by every CPU, and run in turn, each for one tick at most.
 * A thread that sleeps or exits gives its CPU to the next ready thread,
 * and a CPU with nothing to run idles until an interrupt comes. See
 * docs/kernel/processes.md.
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/arch.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <ds/list.h>
#include <dev/random.h>
#include <sys/callout.h>
#include <fs/vfs.h>

#include <aquila/stat.h>

/* The threads ready to run, in the order they will run (their run_link). */
static struct list ready;

/* The processors; cpus[0] is the one that booted. */
struct cpu cpus[CPUS_MAX];
int cpus_count;

/* The threads waiting with a deadline (thread->sleep_until), whichever
 * wait queue they sleep on (their timer_link). */
static struct list timed;

/* nanosleep() sleeps here; only the timer wakes its threads up. */
static struct waitq naps;

/* The processes with a real time timer armed (proc->alarm_at, their
 * alarm_link). */
static struct list alarms;

/**
 * Charges a timer tick to the process that was running on this CPU. It is
 * called on every CPU's tick.
 *
 * The time goes to the process's user time if the tick interrupted user
 * mode, and to its system time otherwise. System calls run with
 * interrupts disabled, so a tick during one waits until it returns, and
 * most kernel time is charged as user time.
 *
 * @param period  the time since this CPU's previous tick, in ns
 * @param user    whether the tick interrupted user mode
 */
void sched_cpu_tick(uint32_t period, int user)
{
    /* a killed thread's process may be gone already (thread->killed) */
    if (!curcpu()->idle && curthread && !curthread->killed) {
        if (user)
            curproc->utime += period;
        else
            curproc->stime += period;
    }
}

/**
 * Runs the boot CPU's timer tick. Besides charging the tick (as on every
 * CPU), it runs the callouts that are due, wakes up the threads whose
 * deadline has passed, and sends `SIGALRM` to the processes whose real
 * time timer has expired.
 *
 * @param period  the time since the previous tick, in ns
 * @param user    whether the tick interrupted user mode
 */
void sched_timer_tick(uint32_t period, int user)
{
    uint64_t now = arch_rtime_ns();

    /* when an interrupt comes, counted in cycles, isn't predictable */
    random_add((uint32_t) arch_cycles());

    callout_run(now);

    sched_cpu_tick(period, user);

    list_for (node, &timed) {
        struct thread *thread = list_entry(node, struct thread, timer_link);

        if (now >= thread->sleep_until) {
            sched_timer_cancel(thread);
            thread_wakeup(thread);
        }
    }

    list_for (node, &alarms) {
        struct proc *proc = list_entry(node, struct proc, alarm_link);

        if (now >= proc->alarm_at) {
            if (proc->alarm_interval) {
                /* the next expiry, rather than every one that was missed */
                proc->alarm_at += proc->alarm_interval;
                if (proc->alarm_at <= now)
                    proc->alarm_at = now + proc->alarm_interval;
            } else {
                list_remove(&proc->alarm_link);
            }

            signal_proc_send(proc, SIGALRM);
        }
    }
}

/**
 * Reads what is left of a process's real time timer.
 *
 * @param proc      the process
 * @param value     set to the time until it expires, in ns, or 0 if it
 *                  isn't armed
 * @param interval  set to its interval, in ns, or 0 if it isn't armed or
 *                  expires only once
 */
void sched_alarm_get(struct proc *proc, uint64_t *value, uint64_t *interval)
{
    uint64_t now = arch_rtime_ns();

    int armed = list_linked(&proc->alarm_link);

    *value = !armed ? 0 : proc->alarm_at > now ? proc->alarm_at - now : 1;
    *interval = armed ? proc->alarm_interval : 0;
}

/**
 * Sets a process's real time timer, which `setitimer()` and `alarm()` use:
 * the process gets `SIGALRM` once `value` ns have passed, then every
 * `interval` ns.
 *
 * @param proc          the process
 * @param value         the time until the first `SIGALRM`, in ns; 0
 *                      disarms the timer
 * @param interval      the time between the next ones, in ns; 0 for none
 * @param old_value     if not NULL, set to what was left of the previous
 *                      timer
 * @param old_interval  if not NULL, set to the previous timer's interval
 */
void sched_alarm(struct proc *proc, uint64_t value, uint64_t interval,
                 uint64_t *old_value, uint64_t *old_interval)
{
    uint64_t now = arch_rtime_ns(), v, i;

    sched_alarm_get(proc, &v, &i);

    if (old_value)
        *old_value = v;

    if (old_interval)
        *old_interval = i;

    list_remove(&proc->alarm_link);

    if (!value)
        return;

    proc->alarm_at = now + value;
    proc->alarm_interval = interval;
    list_append(&alarms, &proc->alarm_link);
}

/**
 * Takes a thread off the list of threads waiting with a deadline, when it
 * no longer waits (it was woken up, or killed).
 *
 * @param thread  the thread
 */
void sched_timer_cancel(struct thread *thread)
{
    list_remove(&thread->timer_link);
}

/**
 * Puts the current thread to sleep on a wait queue until it is woken up,
 * a signal ends the sleep, or a deadline passes.
 *
 * @param waitq  the wait queue to sleep on
 * @param until  the deadline, in `arch_rtime_ns()` time, or 0 for none
 * @return 0 if the thread was woken up, 1 if the deadline passed, or
 *         `-ERESTARTSYS` if a signal ended the sleep
 */
int sched_wait(struct waitq *waitq, uint64_t until)
{
    if (until) {
        if (arch_rtime_ns() >= until)
            return 1;

        curthread->sleep_until = until;
        list_append(&timed, &curthread->timer_link);
    }

    int err = waitq_sleep(waitq);
    sched_timer_cancel(curthread);

    if (err)
        return err;

    return until && arch_rtime_ns() >= until;
}

/**
 * Puts the current thread to sleep until a time comes, for `nanosleep()`.
 *
 * @param until  the time to wake up, in `arch_rtime_ns()` time
 * @return 0, or `-ERESTARTSYS` if a signal ended the sleep before then
 */
int sched_sleep(uint64_t until)
{
    int r;

    while (!(r = sched_wait(&naps, until)))
        ;

    return r < 0 ? r : 0;
}

/**
 * Lets the threads that are ready run before the current one goes on, for
 * `sched_yield()`: the current thread goes to the end of the ready list.
 * If no other thread is ready, it just goes on.
 */
void sched_yield(void)
{
    if (!list_empty(&ready))
        arch_yield();
}

/**
 * Finds the CPU that is running a thread, if it's another CPU than this
 * one.
 *
 * @param thread  the thread
 * @return the CPU, or NULL if the thread isn't running on another CPU
 */
struct cpu *sched_thread_cpu(struct thread *thread)
{
    for (int i = 0; i < cpus_count; ++i)
        if (cpus[i].thread == thread && &cpus[i] != curcpu())
            return &cpus[i];

    return NULL;
}

/**
 * Makes a thread that runs on another CPU enter the kernel now, rather
 * than at its next tick: a signal came for it, or it was killed.
 *
 * @param thread  the thread
 */
void sched_thread_kick(struct thread *thread)
{
    struct cpu *cpu = sched_thread_cpu(thread);

    if (cpu)
        arch_cpu_kick(cpu);
}

/**
 * Ends the current thread, which was killed while it ran on this CPU
 * (`thread->killed`). If its process lives on (an `exec()` ended the
 * thread), the thread leaves it and wakes up the `exec()` that waits for
 * it (`proc_threads_exec()`). It doesn't return.
 */
void sched_thread_killed(void)
{
    struct thread *thread = curthread;

    if (thread->owner) {
        queue_remove(&thread->owner->threads, thread);
        waitq_wakeup(&thread->owner->thread_join);
        thread->owner = NULL;
    }

    arch_cur_thread_kill();
}

/**
 * Wakes up an idle CPU to run a thread that just became ready. If this CPU
 * is the idle one, it is in an interrupt, and runs the thread on the way
 * out of it (`sched_idle_wakeup()`).
 */
static void sched_cpu_wake(void)
{
    if (!sched_running || curcpu()->idle)
        return;

    for (int i = 0; i < cpus_count; ++i) {
        struct cpu *cpu = &cpus[i];

        /* a CPU already woken up runs another thread: try the next one */
        if (cpu != curcpu() && cpu->idle && !cpu->kicked) {
            cpu->kicked = 1;
            arch_cpu_kick(cpu);
            return;
        }
    }
}

/**
 * Puts a thread at the end of the ready list, to run in its turn.
 *
 * @param thread  the thread, which must be on no other list
 */
void sched_thread_ready(struct thread *thread)
{
    list_append(&ready, &thread->run_link);
    sched_cpu_wake();
}

/* An urgent thread was woken up: switch to it when the interrupt returns
 * to user mode (an idle CPU switches anyway). */
int sched_urgent = 0;

/**
 * Makes a thread that was sleeping ready to run. An urgent thread
 * (`thread->urgent`, the network's) goes ahead of the others, and runs as
 * soon as the interrupt that woke it returns.
 *
 * @param thread  the thread that was woken up
 */
void sched_thread_wake(struct thread *thread)
{
    if (!thread->urgent) {
        sched_thread_ready(thread);
        return;
    }

    list_prepend(&ready, &thread->run_link);
    sched_urgent = 1;
    sched_cpu_wake();
}

/**
 * Gives up the CPU when the current thread can't run anymore (it sleeps,
 * or it exited): the next ready thread runs, or the CPU idles until an
 * interrupt comes. It doesn't return to the caller.
 */
void kernel_idle(void)
{
    curcpu()->idle = 1;

    if (!list_empty(&ready))
        schedule();

    arch_idle();
}

/**
 * Runs a ready thread now if an interrupt made one ready while this CPU
 * was idle, rather than at the next tick. Without this, every disk request
 * would wait for a tick.
 */
void sched_idle_wakeup(void)
{
    /* another CPU may wake this one up again */
    curcpu()->kicked = 0;

    if (curcpu()->idle && !list_empty(&ready))
        schedule();
}

/**
 * Starts running a thread for the first time. It doesn't return.
 *
 * @param thread  the thread, which has never run
 */
void sched_thread_spawn(struct thread *thread)
{
    thread->spawned = 1;
    arch_thread_spawn(thread);
}

int sched_running = 0;

/**
 * Starts the scheduler with the first process, init, and runs it. It
 * doesn't return.
 *
 * @param init  the first process, made by the caller
 */
void sched_init_spawn(struct proc *init)
{
    proc_init(init);

    /* init's defaults */
    init->mask = 022;   /* umask */
    init->cwd = vfs_root;
    vfs_root->ref++;

    arch_sched_init();

    session_new(init);

    curthread = (struct thread *) init->threads.head->value;
    curthread->state = RUNNABLE;
    sched_running = 1;
    sched_thread_spawn(curthread);
}

/**
 * Switches to the next ready thread. The running thread, if there is one,
 * goes to the end of the ready list; if no thread is ready, the CPU
 * idles. It returns, in the thread that was running, once that thread is
 * chosen again.
 */
void schedule(void)
{
    sched_urgent = 0;

    if (!curcpu()->idle)
        sched_thread_ready(curthread);

    curcpu()->idle = 0;

    if (list_empty(&ready))
        kernel_idle();

    curthread = list_entry(list_pop(&ready), struct thread, run_link);

    if (curthread->spawned) {
        arch_thread_switch(curthread);
    } else {
        sched_thread_spawn(curthread);
    }
}
