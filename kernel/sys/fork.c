/** \file
 * `fork()`: a copy of the calling process.
 *
 * The child gets copies of the parent's process structure, descriptors
 * (referring to the same open files) and address space. The memory isn't
 * copied: both processes share its pages read-only, and the first one to
 * write a page gets its own copy (copy on write, `vm_space_fork()`). Only
 * the calling thread is copied.
 */

#include <core/system.h>
#include <core/string.h>
#include <core/arch.h>
#include <mm/mm.h>
#include <mm/vm.h>
#include <mm/pmap.h>
#include <sys/proc.h>
#include <ds/queue.h>
#include <aquila/errno.h>
#include <net/socket.h>

MALLOC_DECLARE(M_FDS);

/** Copies the parent's descriptors to the child. Each refers to the same
 * open file, so the two processes share its offset. Returns 0 or
 * `-ENOMEM`. */
static int copy_fds(struct proc *parent, struct proc *fork)
{
    fork->fds = kmalloc(FDS_COUNT * sizeof(struct fd), &M_FDS, M_ZERO);

    if (!fork->fds)
        return -ENOMEM;

    for (int i = 0; i < FDS_COUNT; ++i) {
        /* a descriptor that another thread is still setting up isn't
         * copied */
        struct file *file = proc_fd_file(parent, i);

        if (file)
            fork->fds[i] = (struct fd) {.file = file_ref(file), .flags = parent->fds[i].flags};
    }

    return 0;
}

/** Copies what the child inherits from the parent's process structure:
 * its process group, umask, IDs, signal actions, and heap.
 * Returns 0, or `-ENOMEM` if there was no memory to put the child in the
 * process group. */
static int fork_proc_copy(struct proc *parent, struct proc *fork)
{
    fork->pgrp = parent->pgrp;

    if (!(fork->pgrp_node = enqueue(fork->pgrp->procs, fork)))
        return -ENOMEM;

    fork->mask = parent->mask;
    fork->uid  = parent->uid;
    fork->gid  = parent->gid;

    fork->heap_start = parent->heap_start;
    fork->heap  = parent->heap;
    fork->entry = parent->entry;

    memcpy(fork->sigaction, parent->sigaction, sizeof(parent->sigaction));

    return 0;
}

/**
 * Makes a child process, a copy of the process of `thread`. The child's
 * one thread is a copy of `thread`, which will return 0 from `fork()`; it
 * isn't made ready to run, the caller does that.
 *
 * @param thread  the thread calling `fork()`
 * @param ref     if not NULL, set to the child
 * @return 0, or a negative errno: `-ENOMEM`, or `-EAGAIN` if there is no
 *         free process ID
 */
int proc_fork(struct thread *thread, struct proc **ref)
{
    int err = 0;
    struct proc *fork = NULL;
    struct thread *fork_thread = NULL;

    /* the new process */
    if ((err = proc_new(&fork)))
        goto error;

    /* its main thread */
    fork_thread = (struct thread *) fork->threads.head->value;

    /* the parent */
    struct proc *proc = thread->owner;

    /* what the child inherits from the parent's structure */
    if ((err = fork_proc_copy(proc, fork)))
        goto error;

    /* the parent's name */
    if (!(fork->name = strdup(proc->name))) {
        err = -ENOMEM;
        goto error;
    }

    /* a new process ID */
    if ((fork->pid = proc_pid_alloc()) == -1) {
        fork->pid = 0;
        err = -EAGAIN;
        goto error;
    }

    /* the parent, for wait() */
    fork->parent = proc;

    /* the child's thread starts as if it had been switched away from:
     * from where fork() returns; with the signal mask of the thread that
     * forked, and nothing pending */
    fork_thread->spawned = 1;
    fork_thread->sigmask = thread->sigmask;

    /* the same current directory */
    if ((fork->cwd = proc->cwd))
        fork->cwd->ref++;

    /* the descriptors */
    if ((err = copy_fds(proc, fork)))
        goto error;

    /* the address space, copy on write */
    if ((err = vm_space_fork(&proc->vm_space, &fork->vm_space)))
        goto error;

    /* point the child's heap_vm and stack_vm at its copies of the
     * parent's: the copies are in the same order as the originals */
    struct qnode *pvm_node = proc->vm_space.vm_entries.head;
    struct qnode *fvm_node = fork->vm_space.vm_entries.head;

    while (pvm_node) {
        struct vm_entry *pvm_entry = pvm_node->value;
        struct vm_entry *fvm_entry = fvm_node->value;

        if (pvm_entry == proc->heap_vm)
            fork->heap_vm = fvm_entry;

        if (pvm_entry == proc->stack_vm)
            fork->stack_vm = fvm_entry;

        pvm_node = pvm_node->next;
        fvm_node = fvm_node->next;
    }

    /* the thread's CPU state, a copy of the caller's */
    err = arch_proc_fork(thread, fork);

    if (err)
        goto error;

    /* fork() returns 0 in the child, set in its registers since it is
     * another thread (the parent gets the child's PID: sys_fork()'s
     * result) */
    arch_syscall_return(fork_thread, 0);

    if (ref)
        *ref = fork;

    return 0;

error:
    /* undo what was done; the caller returns err to the parent */
    if (fork) {
        /* close the copied descriptors (drops the references they took) */
        if (fork->fds) {
            for (int i = 0; i < FDS_COUNT; ++i)
                proc_fd_close(fork, i);

            kfree(fork->fds);
        }

        vm_space_destroy(&fork->vm_space);
        pmap_decref(fork->vm_space.pmap);

        if (fork->name)
            kfree(fork->name);
        if (fork->cwd)
            vfs_close(fork->cwd);

        /* free the main thread (it never ran) */
        struct thread *_thread = NULL;
        while ((_thread = dequeue(&fork->threads)))
            kfree(_thread);

        if (fork->pgrp_node)
            queue_node_remove(fork->pgrp->procs, fork->pgrp_node);

        if (fork->pid > 0)
            proc_pid_free(fork->pid);

        queue_remove(procs, fork);
        kfree(fork);
    }

    return err;
}
