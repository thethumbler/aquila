/** \file
 * Threads: making one in a process, and ending one. Running them is the
 * scheduler's (`sys/sched.c`), sleeping and waking up the wait queues'
 * (`sys/waitq.c`), and the CPU state the architecture's
 * (`arch_thread_create()`, `arch_thread_kill()`).
 */

#include <core/system.h>
#include <core/arch.h>
#include <core/panic.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <ds/queue.h>

MALLOC_DEFINE(M_THREAD, "thread", "thread structure");

/**
 * Makes a new thread in a process. The thread has an ID but no CPU state
 * yet, and doesn't run: `thread_create()` and `fork()` set it up.
 *
 * @param proc  the process the thread belongs to
 * @param ref   if not NULL, set to the new thread
 * @return 0, `-EINVAL` if `proc` is NULL, or `-ENOMEM` if there was no
 *         memory for the thread
 */
int thread_new(struct proc *proc, struct thread **ref)
{
    struct thread *thread = NULL;

    if (!proc)
        return -EINVAL;

    thread = kmalloc(sizeof(struct thread), &M_THREAD, M_ZERO);
    if (!thread) return -ENOMEM;

    thread->owner = proc;
    thread->tid = ++proc->tid_last;

    if (!enqueue(&proc->threads, thread)) {
        kfree(thread);
        return -ENOMEM;
    }

    if (ref)
        *ref = thread;

    return 0;
}

/**
 * Releases what a thread holds (its `poll()` registrations, its CPU state
 * and kernel stack) and makes it a zombie. The thread's structure itself
 * is freed by its caller, or by `thread_join()`.
 *
 * @param thread  the thread, which must not be running
 * @return 0, or `-EINVAL` if `thread` is NULL
 */
int thread_kill(struct thread *thread)
{
    if (!thread)
        return -EINVAL;

    /* release its resources */
    poll_unregister(thread);
    arch_thread_kill(thread);
    thread->state = ZOMBIE;
    return 0;
}

/**
 * Finds a thread of a process by its ID.
 *
 * @param proc  the process
 * @param tid   the thread's ID
 * @return the thread, a zombie or not, or NULL if the process has none
 *         with that ID
 */
struct thread *thread_find(struct proc *proc, tid_t tid)
{
    queue_for (node, &proc->threads) {
        struct thread *thread = node->value;

        if (thread->tid == tid)
            return thread;
    }

    return NULL;
}

/**
 * Makes a new thread in the process of an existing one, for
 * `pthread_create()`. The new thread is set up to start at `uentry` in
 * user mode, with the signal mask of `thread` and no pending signals, but
 * isn't made ready to run: the caller does that.
 *
 * @param thread      a thread of the process to add the new one to
 * @param stack       the new thread's user stack
 * @param entry       where the new thread starts in user mode (the C
 *                    library's thread start)
 * @param uentry      the function the C library's start calls
 * @param arg         the argument it passes to `uentry`
 * @param attr        not used
 * @param new_thread  if not NULL, set to the new thread
 * @return 0, or a negative errno
 */
int thread_create(struct thread *thread, uintptr_t stack, uintptr_t entry, uintptr_t uentry, uintptr_t arg, uintptr_t attr __unused, struct thread **new_thread)
{
    int err = 0;
    struct thread *t = NULL;

    if ((err = thread_new(thread->owner, &t)))
        return err;

    if ((err = arch_thread_create(t, stack, entry, uentry, arg))) {
        queue_remove(&thread->owner->threads, t);
        kfree(t);
        return err;
    }

    t->sigmask = thread->sigmask;

    if (new_thread)
        *new_thread = t;

    return 0;
}
