/** \file
 * Processes: making them and ending them, their IDs, their descriptors,
 * and their sessions and process groups.
 *
 * A process that ends becomes a zombie: it keeps its ID and its exit
 * status until its parent waits for it (`proc_reap()`). Its children go to
 * init. Sessions and process groups follow POSIX's job control: a group
 * that would be left stopped with no one to continue it (an orphaned
 * group) gets `SIGHUP` and `SIGCONT`. See docs/kernel/processes.md.
 */


#include <core/system.h>
#include <core/panic.h>
#include <core/string.h>
#include <core/arch.h>

#include <mm/pmap.h>
#include <mm/mm.h>
#include <mm/vm.h>

#include <sys/proc.h>
#include <sys/sched.h>

#include <fs/vfs.h>
#include <dev/tty.h>

#include <ds/queue.h>
#include <ds/bitmap.h>

MALLOC_DEFINE(M_PROC, "proc", "process structure");
MALLOC_DEFINE(M_SESSION, "session", "session structure");
MALLOC_DEFINE(M_PGROUP, "pgroup", "process group structure");
MALLOC_DEFINE(M_FDS, "fds", "file descriptor array");

/* every process */
struct queue *procs = QUEUE_NEW();

struct proc *initproc;

/* every session (each has its process groups) */
struct queue *sessions = QUEUE_NEW();

/* the process IDs in use */
struct bitmap *pid_bitmap = BITMAP_NEW(4096);

/* The last process ID given out. The next one is the first free ID after
 * it, wrapping around, as UNIX does: the ID of a process just reaped isn't
 * given out again at once, so a kill() meant for it doesn't find a new
 * process. */
static int last_pid;

/** Returns whether a process group or a session has `pid` as its ID. Such
 * an ID isn't given to a new process (POSIX), since groups and sessions
 * outlive their leader. */
static int pid_is_group(int pid)
{
    queue_for (snode, sessions) {
        struct session *session = snode->value;

        if (session->sid == pid)
            return 1;

        queue_for (gnode, session->pgps) {
            struct pgroup *pgrp = gnode->value;

            if (pgrp->pgid == pid)
                return 1;
        }
    }

    return 0;
}

/**
 * Allocates a process ID, from 1 (init's) on.
 *
 * @return the ID, or -1 if none is free
 */
int proc_pid_alloc()
{
    size_t range = pid_bitmap->max_idx - 1;     /* 1 to max_idx - 1 */

    for (size_t n = 0; n < range; ++n) {
        int pid = 1 + (last_pid + n) % range;

        if (!bitmap_check(pid_bitmap, pid) && !pid_is_group(pid)) {
            bitmap_set(pid_bitmap, pid);
            last_pid = pid;
            return pid;
        }
    }

    return -1;
}

/**
 * Frees a process ID.
 *
 * @param pid  the ID
 */
void proc_pid_free(int pid)
{
    bitmap_clear(pid_bitmap, pid);
}

/**
 * Makes a new process structure, with one thread and an empty address
 * space, on the list of every process. It has no ID yet.
 *
 * @param ref  if not NULL, set to the new process
 * @return 0, or `-ENOMEM`
 */
int proc_new(struct proc **ref)
{
    int err = 0;
    struct proc *proc = NULL;
    struct thread *thread = NULL;
    struct pmap *pmap = NULL;

    proc = kmalloc(sizeof(struct proc), &M_PROC, M_ZERO);

    if (!proc) {
        err = -ENOMEM;
        goto error;
    }

    err = thread_new(proc, &thread);

    if (err != 0)
        goto error;

    pmap = pmap_create();

    if (pmap == NULL) {
        err = -ENOMEM;
        goto error;
    }

    proc->vm_space.pmap = pmap;

    /* every signal has its default action */
    for (int i = 0; i < SIG_MAX; ++i)
        proc->sigaction[i].sa_handler = SIG_DFL;

    proc->running = 1;

    if (!enqueue(procs, proc)) {
        err = -ENOMEM;
        goto error;
    }

    if (ref)
        *ref = proc;

    return 0;

error:
    if (pmap)
        pmap_decref(pmap);

    if (thread) {
        queue_remove(&proc->threads, thread);
        kfree(thread);
    }

    if (proc)
        kfree(proc);

    return err;
}

/**
 * Finds a process by its ID.
 *
 * @param pid  the ID
 * @return the process (it may be a zombie), or NULL
 */
struct proc *proc_pid_find(pid_t pid)
{
    queue_for (node, procs) {
        struct proc *proc = node->value;
        if (proc->pid == pid)
            return proc;
    }
    
    return NULL;
}

/**
 * Gives a new process (init) its ID and its descriptor table.
 *
 * @param proc  the process
 * @return 0, `-EINVAL` if `proc` is NULL, `-EAGAIN` if no ID is free, or
 *         `-ENOMEM`
 */
int proc_init(struct proc *proc)
{
    if (!proc)
        return -EINVAL;

    /* no free ID: fork() says to try again */
    if ((proc->pid = proc_pid_alloc()) < 0)
        return -EAGAIN;

    proc->fds = kmalloc(FDS_COUNT * sizeof(struct fd), &M_FDS, M_ZERO);

    if (!proc->fds) {
        proc_pid_free(proc->pid);
        return -ENOMEM;
    }

    return 0;
}

/**
 * Says whether a process group is orphaned (POSIX): none of its members
 * has a parent in another group of the same session, so no one there
 * would continue it once it stops. Init doesn't count as such a parent,
 * since it isn't a shell.
 *
 * @param pgrp  the process group
 * @param gone  a process on its way out, which doesn't count as a member
 *              or a parent; or NULL
 * @return 1 if it is orphaned, 0 if not
 */
int pgrp_orphaned(struct pgroup *pgrp, struct proc *gone)
{
    queue_for (node, pgrp->procs) {
        struct proc *p = node->value, *parent = p->parent;

        if (p == gone || p->zombie || !parent || parent == gone || parent == initproc)
            continue;

        if (parent->pgrp != pgrp && parent->pgrp->session == pgrp->session)
            return 0;
    }

    return 1;
}

/** Sends `SIGHUP` and `SIGCONT` to a process group that `gone` leaving
 * makes orphaned, if one of its members is stopped: nothing else would
 * continue them. */
static void pgrp_orphaned_hangup(struct pgroup *pgrp, struct proc *gone)
{
    int stopped = 0;

    queue_for (node, pgrp->procs) {
        struct proc *p = node->value;

        if (p != gone && p->running && p->stopped)
            stopped = 1;
    }

    if (stopped && pgrp_orphaned(pgrp, gone)) {
        signal_pgrp_send(pgrp, SIGHUP);
        signal_pgrp_send(pgrp, SIGCONT);
    }
}

/** Returns whether `p`'s process group is another group of the same
 * session as `q`'s. */
static int pgrp_other_same_session(struct proc *p, struct proc *q)
{
    return p->pgrp != q->pgrp && p->pgrp->session == q->pgrp->session;
}

/** Tells a zombie's parent that it ended: its `wait()` wakes up and it gets
 * `SIGCHLD`. If the parent doesn't wait for its children (it ignores
 * `SIGCHLD`, or set `SA_NOCLDWAIT`), the zombie is reaped at once. */
static void proc_zombie_notify(struct proc *proc)
{
    struct proc *parent = proc->parent;

    if (!parent) {
        proc_reap(proc);
        return;
    }

    int nowait = parent->sigaction[SIGCHLD].sa_handler == SIG_IGN ||
                 (parent->sigaction[SIGCHLD].sa_flags & SA_NOCLDWAIT);

    /* how it ended: an exit code, or the signal that ended it */
    int sig = proc->exit & 0xff;
    struct aquila_siginfo info = {
        .si_signo  = SIGCHLD,
        .si_code   = sig ? CLD_KILLED : CLD_EXITED,
        .si_pid    = proc->pid,
        .si_uid    = proc->uid,
        .si_status = sig ? sig : (proc->exit >> 8) & 0xff,
    };

    waitq_wakeup(&parent->wait_queue);
    signal_post(parent, NULL, &info);

    if (nowait)
        proc_reap(proc);
}

/**
 * Ends the threads of a process, but one. A thread that isn't running is
 * freed. One running on another CPU (in user mode, since this CPU has the
 * kernel lock) is marked killed and made to enter the kernel now, where it
 * frees itself.
 *
 * @param proc     the process
 * @param keep     the thread to keep, or NULL
 * @param exiting  whether the process is ending: then the threads running
 *                 elsewhere are detached from it at once; otherwise they
 *                 leave its list when they go, and wake up
 *                 `proc->thread_join`
 * @return how many threads were running on other CPUs
 */
int proc_threads_end(struct proc *proc, struct thread *keep, int exiting)
{
    int elsewhere = 0;

    for (struct qnode *node = proc->threads.head, *next; node; node = next) {
        struct thread *thread = node->value;
        next = node->next;

        if (thread == keep)
            continue;

        if (sched_thread_cpu(thread)) {
            thread->killed = 1;
            sched_thread_kick(thread);
            elsewhere++;

            if (exiting) {
                queue_node_remove(&proc->threads, node);
                thread->owner = NULL;
            }

            continue;
        }

        queue_node_remove(&proc->threads, node);

        /* take it off the wait queue it sleeps on, or the ready list */
        list_remove(&thread->run_link);
        thread->waitq = NULL;

        sched_timer_cancel(thread);

        /* it won't return from its system call: let go of the files the
         * call held */
        thread_files_release(thread);

        /* a zombie thread (thread_exit()) released its resources already */
        if (thread->state != ZOMBIE)
            thread_kill(thread);

        kfree(thread);
    }

    return elsewhere;
}

/**
 * Ends every thread of a process but the current one, for `execve()`
 * (POSIX: they run in the program that is replaced). It waits for the
 * threads running on other CPUs to be gone.
 *
 * @param proc  the process, which must be the current thread's
 */
void proc_threads_exec(struct proc *proc)
{
    if (!curthread || curthread->owner != proc)
        return;

    while (proc_threads_end(proc, curthread, 0))
        waitq_sleep_unint(&proc->thread_join);
}

/**
 * Ends a process: its threads end, its descriptors are closed, its memory
 * is freed, its children go to init, and it becomes a zombie that its
 * parent may wait for. If the current thread is the process's, it doesn't
 * return. Ending init restarts the machine.
 *
 * @param proc  the process; its exit status is set already
 */
void proc_kill(struct proc *proc)
{
    if (proc->pid == 1) {
        if (proc->exit)
            panic("init killed");

        printk("kernel: reached target reboot\n");
        arch_reboot();

        panic("reboot not implemented\n");
    }

    proc->running = 0;

    /* no more SIGALRM */
    sched_alarm(proc, 0, 0, NULL, NULL);

    /* its threads end, the current one last (it runs this code) */
    struct thread *self = curthread && curthread->owner == proc ? curthread : NULL;
    int kill_curthread = self != NULL;

    proc_threads_end(proc, self, 1);

    if (self) {
        queue_remove(&proc->threads, self);
        thread_files_release(self);
    }

    /* close every descriptor */
    for (int i = 0; i < FDS_COUNT; ++i)
        proc_fd_close(proc, i);

    /* what follows frees what a thread killing its own process runs on
     * (its address space, and its process once reaped), so it must not be
     * switched out until it is gone */
    if (kill_curthread)
        arch_interrupts_disable();

    struct vm_space *vm_space = &proc->vm_space;
    vm_space_destroy(vm_space);
    pmap_decref(vm_space->pmap);

    /* nothing may switch to the freed page tables */
    vm_space->pmap = kvm_space.pmap;

    /* the descriptor table and the working directory */
    kfree(proc->fds);

    if (proc->cwd)
        vfs_close(proc->cwd);

    proc->cwd = NULL;

    proc->sig_pending = 0;

    /* its own group: its parent, in another group of the session, may
     * have been what kept the group from being orphaned */
    if (proc->parent && pgrp_other_same_session(proc->parent, proc))
        pgrp_orphaned_hangup(proc->pgrp, proc);

    /* its children become init's: init waits for them (for the zombies
     * among them now, for the others when they end). A child's group, if
     * it is another group of the session, may be orphaned now */
    struct qnode *node = procs->head;
    while (node) {
        struct proc *_proc = node->value;
        node = node->next;  /* proc_reap() removes the current node */

        if (_proc->parent == proc) {
            _proc->parent = initproc;

            if (pgrp_other_same_session(_proc, proc))
                pgrp_orphaned_hangup(_proc->pgrp, proc);

            if (_proc->zombie)
                proc_zombie_notify(_proc);
        }
    }

    /* if it leads its session, the session ends and its controlling
     * terminal hangs up (a zombie stays in its process group until it is
     * reaped: waitpid() matches it by group) */
    struct session *session = proc->pgrp->session;

    if (session->leader == proc) {
        if (session->ctty)
            tty_hangup(session->ctty);

        session->leader = NULL;
    }

    proc->zombie = 1;

    /* tell its parent, which may wait for it */
    proc_zombie_notify(proc);

    if (kill_curthread) {
        arch_cur_thread_kill();
        panic("How did we get here?");
    }
}

/**
 * Frees a zombie, once its parent waited for it (or doesn't wait for its
 * children). Its CPU times are added to its parent's children's times.
 *
 * @param proc  the zombie
 * @return 0
 */
int proc_reap(struct proc *proc)
{
    /* its CPU times count as the parent's children's */
    if (proc->parent) {
        proc->parent->cutime += proc->utime + proc->cutime;
        proc->parent->cstime += proc->stime + proc->cstime;
    }

    proc_pid_free(proc->pid);

    queue_node_remove(proc->pgrp->procs, proc->pgrp_node);
    pgrp_release(proc->pgrp);

    /* the name stayed with the zombie until now (ps and /proc show it) */
    kfree(proc->name);

    queue_remove(procs, proc);
    kfree(proc);

    return 0;
}

/**
 * Reserves the lowest free descriptor of a process, while a system call
 * sets it up: no other thread takes it, and it doesn't refer to a file
 * yet. `proc_fd_install()` or `proc_fd_release()` ends the reservation.
 *
 * @param proc  the process
 * @return the descriptor, or -1 if there is no free one
 */
int proc_fd_get(struct proc *proc)
{
    for (int i = 0; i < FDS_COUNT; ++i) {
        if (!proc->fds[i].file) {
            proc->fds[i] = (struct fd) {.file = FD_RESERVED};
            return i;
        }
    }

    return -1;
}

/**
 * Frees a reserved descriptor that wasn't set up after all.
 *
 * @param proc  the process
 * @param fd    the descriptor
 */
void proc_fd_release(struct proc *proc, int fd)
{
    if (fd >= 0 && fd < FDS_COUNT)
        proc->fds[fd] = (struct fd) {0};
}

/**
 * Makes a reserved descriptor refer to an open file. The caller's
 * reference to the file becomes the descriptor's.
 *
 * @param proc   the process
 * @param fd     the descriptor, reserved by `proc_fd_get()`
 * @param file   the open file
 * @param flags  the descriptor's flags (`FD_CLOEXEC`)
 */
void proc_fd_install(struct proc *proc, int fd, struct file *file, int flags)
{
    proc->fds[fd] = (struct fd) {.file = file, .flags = flags};
}

/**
 * Finds the open file a descriptor refers to.
 *
 * @param proc  the process
 * @param fd    the descriptor
 * @return the open file, or NULL if the descriptor is free, being set up,
 *         or out of range
 */
struct file *proc_fd_file(struct proc *proc, int fd)
{
    if (fd < 0 || fd >= FDS_COUNT)
        return NULL;

    struct file *file = proc->fds[fd].file;
    return file == FD_RESERVED ? NULL : file;
}

/**
 * Closes a descriptor: it is free again, and its reference to the open
 * file is dropped. Dropping the last reference closes the file.
 *
 * @param proc  the process
 * @param fd    the descriptor
 * @return what closing the file returned (0 or a negative errno), or
 *         `-EBADF` if the descriptor isn't open
 */
int proc_fd_close(struct proc *proc, int fd)
{
    struct file *file = proc_fd_file(proc, fd);

    if (!file)
        return -EBADF;

    proc->fds[fd] = (struct fd) {0};
    return file_put(file);
}

/**
 * Makes a new session, with a new process group, both led by a process
 * and with its ID, and moves the process into it, as `setsid()` does.
 *
 * @param proc  the process
 * @return 0, or `-ENOMEM`
 */
int session_new(struct proc *proc)
{
    int err = 0;
    struct session *session = NULL;
    struct pgroup  *pgrp    = NULL;

    /* the session */
    session = kmalloc(sizeof(struct session), &M_SESSION, M_ZERO);
    if (!session) goto e_nomem;

    /* its process group */
    pgrp = kmalloc(sizeof(struct pgroup), &M_PGROUP, M_ZERO);
    if (!pgrp) goto e_nomem;

    session->pgps = queue_new();
    if (!session->pgps) goto e_nomem;

    pgrp->procs = queue_new();
    if (!pgrp->procs) goto e_nomem;

    pgrp->session_node = enqueue(session->pgps, pgrp);
    if (!pgrp->session_node) goto e_nomem;

    /* the process's place in its old group is kept until this can't fail
     * anymore (setsid() takes it back on failure) */
    struct qnode *node = enqueue(pgrp->procs, proc);
    if (!node) goto e_nomem;

    proc->pgrp_node = node;

    session->sid = proc->pid;
    pgrp->pgid = proc->pid;

    session->leader = proc;
    pgrp->leader = proc;

    pgrp->session = session;
    proc->pgrp = pgrp;

    return 0;

e_nomem:
    err = -ENOMEM;
error:
    if (session) {
        /* the group may be on the session's list already */
        if (pgrp && pgrp->session_node)
            queue_node_remove(session->pgps, pgrp->session_node);

        kfree(session->pgps);
        kfree(session);
    }

    if (pgrp) {
        kfree(pgrp->procs);
        kfree(pgrp);
    }

    return err;
}

/**
 * Moves a process into a process group of its session, as `setpgid()`
 * does. A group can only be joined if it exists and has members; a
 * process can only start a new group with its own ID as the group's ID.
 *
 * @param proc  the process
 * @param pgid  the group's ID
 * @return 0, `-EPERM` if there is no such group and `pgid` isn't the
 *         process's ID, or `-ENOMEM`
 */
int proc_setpgid(struct proc *proc, pid_t pgid)
{
    struct session *session = proc->pgrp->session;
    struct pgroup *pgrp = NULL;
    int made = 0;

    if (proc->pgrp->pgid == pgid)
        return 0;

    queue_for (node, session->pgps) {
        struct pgroup *pg = node->value;

        if (pg->pgid == pgid) {
            pgrp = pg;
            break;
        }
    }

    if ((!pgrp || !pgrp->procs->count) && pgid != proc->pid)
        return -EPERM;

    if (!pgrp) {
        if (!(pgrp = kmalloc(sizeof(struct pgroup), &M_PGROUP, M_ZERO)))
            return -ENOMEM;

        pgrp->pgid    = pgid;
        pgrp->session = session;

        if (!(pgrp->procs = queue_new())) {
            kfree(pgrp);
            return -ENOMEM;
        }

        if (!(pgrp->session_node = enqueue(session->pgps, pgrp))) {
            kfree(pgrp->procs);
            kfree(pgrp);
            return -ENOMEM;
        }

        made = 1;
    }

    struct qnode *node = enqueue(pgrp->procs, proc);

    if (!node) {
        /* a group made for the process goes again, empty */
        if (made) {
            queue_node_remove(session->pgps, pgrp->session_node);
            kfree(pgrp->procs);
            kfree(pgrp);
        }

        return -ENOMEM;
    }

    if (pgid == proc->pid)
        pgrp->leader = proc;

    struct pgroup *old = proc->pgrp;

    queue_node_remove(old->procs, proc->pgrp_node);
    proc->pgrp_node = node;
    proc->pgrp = pgrp;

    pgrp_release(old);
    return 0;
}

/**
 * Frees a process group if no process is left in it, and its session if
 * that was the session's last group. A terminal whose foreground group or
 * session it was loses it.
 *
 * @param pgrp  the process group
 */
void pgrp_release(struct pgroup *pgrp)
{
    if (pgrp->procs->count)
        return;

    struct session *session = pgrp->session;

    if (session->ctty && session->ctty->fg == pgrp)
        session->ctty->fg = NULL;

    queue_node_remove(session->pgps, pgrp->session_node);
    kfree(pgrp->procs);
    kfree(pgrp);

    if (session->pgps->count)
        return;

    if (session->ctty) {
        session->ctty->session = NULL;
        session->ctty->fg = NULL;
    }

    kfree(session->pgps);
    kfree(session);
}
