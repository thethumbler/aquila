#ifndef _SYS_PROC_H
#define _SYS_PROC_H

#include <core/system.h>

struct proc;
struct pgroup;
struct session;

#include <mm/vm.h>
#include <fs/vfs.h>
#include <ds/queue.h>
#include <sys/waitq.h>
#include <sys/thread.h>
#include <sys/signal.h>
#include <dev/dev.h>

struct tty;

/*
 * Processes, their process groups and sessions (sys/proc.c). See
 * docs/kernel/processes.md.
 */

/** A session: its process groups, and its controlling terminal. */
struct session {
    /** the session's ID */
    pid_t sid;

    /** its process groups */
    struct queue *pgps;

    /** its leader, the process that made it */
    struct proc *leader;

    /** its controlling terminal, or NULL */
    struct tty *ctty;

    /** its node on the list of sessions */
    struct qnode *qnode;
};

/**
 * A file descriptor: the open file it refers to (fs/file.c, shared with
 * the descriptors that dup() and fork() copied), and its own flags.
 */
struct fd {
    struct file *file;  /* NULL when it's free, FD_RESERVED while it's being set up */
    int flags;          /* FD_CLOEXEC */
};

/* A descriptor that's taken, but not set up yet. */
#define FD_RESERVED ((struct file *) -1)

/** A process group: the processes of a job. */
struct pgroup {
    /** the group's ID */
    pid_t pgid;

    /** its session */
    struct session *session;

    /** its node on its session's list */
    struct qnode *session_node;

    /** its processes */
    struct queue *procs;

    /** its leader, the process whose PID is its ID */
    struct proc *leader;

};

/** A process. */
struct proc {
    /** the process's ID */
    pid_t pid;

    /** its process group, and its node on the group's list */
    struct pgroup *pgrp;
    struct qnode  *pgrp_node;

    /** its name: the program it runs */
    char *name;

    /** its file descriptors (FDS_COUNT of them) */
    struct fd *fds;

    /** its parent */
    struct proc *parent;

    /** its current directory (a reference), or NULL for the root */
    struct vnode *cwd;

    /** its file mode creation mask (umask) */
    mode_t mask;

    /** the signal that stopped it, or 0; and what waitpid() has to report
     *  (WAIT_*) */
    int stopped;
    int wait_event;

    /** its stopped threads sleep here */
    struct waitq stop_queue;

    /** its CPU time in user mode and in the kernel (ns), and that of the
     *  children it waited for */
    uint64_t utime, stime;
    uint64_t cutime, cstime;

    /** its real time timer (ITIMER_REAL): SIGALRM when arch_rtime_ns()
     *  reaches alarm_at, then every alarm_interval ns (0: only once); on
     *  the scheduler's alarms list while it's armed */
    uint64_t alarm_at, alarm_interval;
    struct list_node alarm_link;

    /** its user and group IDs */
    uid_t uid;
    gid_t gid;

    /** where its heap starts, and where it ends now (sbrk()) */
    uintptr_t heap_start;
    uintptr_t heap;

    /** its program's entry point */
    uintptr_t entry;

    /** its address space */
    struct vm_space vm_space;

    /** its heap's and its stack's mappings */
    struct vm_entry *heap_vm;
    struct vm_entry *stack_vm;

    /** its threads */
    struct queue threads;

    /** the last thread ID given out (threads leave in any order, so their
     *  count can't be the next one's ID) */
    int tid_last;

    /** threads waiting in thread_join() sleep here */
    struct waitq thread_join;

    /** the signals pending on the process (kill()), a bit (1 << sig)
     *  each, which any of its threads that doesn't block one may take; a
     *  signal sent again while it's pending is delivered once (POSIX
     *  leaves this to us, and everyone does it). Each thread has its own
     *  pending signals and mask too (struct thread) */
    sigset_t sig_pending;

    /** each pending signal's siginfo: the first sending's */
    struct aquila_siginfo sig_info[SIG_MAX+1];

    /** a parent waiting for a child (waitpid()) sleeps here */
    struct waitq wait_queue;

    /** its signal actions */
    struct aquila_sigaction sigaction[SIG_MAX+1];

    /** its exit status (PROC_EXIT()) */
    int exit;

    /** whether it's running */
    int running;

    /** proc_kill() is done with it, so it can be reaped (that isn't as
     *  soon as it stops running: closing its files may sleep) */
    int zombie;
};

/* sys/fork.c: copies the process of `thread` */
int proc_fork(struct thread *thread, struct proc **ref);

/* sys/execve.c: loads a program into the process of `thread` */
int proc_execve(struct thread *thread, const char *fn, char * const argv[], char * const env[]);

/* Sets up init, the first process, to start its program, at boot. */
void init_execve(struct proc *proc, int argc, char * const argv[], int envc, char * const envp[]);

/* sys/proc.c: process IDs, making and ending processes, groups and
 * sessions, file descriptors */
pid_t proc_pid_alloc(void);
void  proc_pid_free(int pid);
struct proc *proc_pid_find(pid_t pid);

int proc_new(struct proc **ref);
int session_new(struct proc *proc);
int proc_setpgid(struct proc *proc, pid_t pgid);
void pgrp_release(struct pgroup *pgrp);
int  pgrp_orphaned(struct pgroup *pgrp, struct proc *gone);

void proc_kill(struct proc *proc);
int  proc_threads_end(struct proc *proc, struct thread *keep, int exiting);
void proc_threads_exec(struct proc *proc);
int  proc_reap(struct proc *proc);
int  proc_fd_get(struct proc *proc);
void proc_fd_release(struct proc *proc, int fd);
void proc_fd_install(struct proc *proc, int fd, struct file *file, int flags);
struct file *proc_fd_file(struct proc *proc, int fd);
int  proc_fd_close(struct proc *proc, int fd);
void proc_dump(struct proc *proc);

int  proc_init(struct proc *proc);

/* An exit status, as waitpid() reports it: `info` (the exit code) in the
 * second byte, and `code` (the signal that ended the process, or 0) in
 * the first. */
#define PROC_EXIT(info, code) ((((info) & 0xff) << 8) | ((code) & 0xff))

/* A process's uio: its current directory and credentials. */
#define PROC_UIO(proc) ((struct uio){.cwd = (proc)->cwd, .uid = (proc)->uid, .gid = (proc)->gid, .mask = (proc)->mask})

/* All the processes. */
extern struct queue *procs;

/* init (PID 1), the orphans' parent; only it and the kernel's process have
 * no parent. And the sessions. */
extern struct proc *initproc;
extern struct queue *sessions;

#endif /* ! _SYS_PROC_H */
