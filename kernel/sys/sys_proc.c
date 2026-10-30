/** \file
 * System calls about processes and threads: `exit()`, `execve()`,
 * `fork()`, `waitpid()`, threads, IDs, process groups and sessions,
 * memory (`sbrk()`, `mmap()`, `munmap()`), and the machine (`uname()`,
 * `reboot()`, `sethostname()`).
 */

#include "syscalls.h"

/**
 * Implements `_exit()`: ends the calling process. It doesn't return.
 *
 * @param code  the exit status, which the parent's `wait()` reports
 * @return nothing: it doesn't return
 */
long sys_exit(int code)
{
    syscall_log(LOG_DEBUG, "exit(code=%d)\n", code);

    struct proc *owner = curproc;

    owner->exit = PROC_EXIT(code, 0);  /* exited normally, not by a signal */

    proc_kill(owner);
    arch_sleep();

    /* not reached: the process is gone, this thread with it */
    for (;;);
    return 0;
}

/**
 * Implements `execve()`: runs another program in the calling process.
 *
 * @param path  the program's path
 * @param argp  its arguments, a NULL-terminated array
 * @param envp  its environment, a NULL-terminated array
 * @return a negative errno on failure (`-ENOENT`, `-EACCES`, `-ENOEXEC`,
 *         `-E2BIG`, `-EFAULT`, `-ENOMEM`); on success it doesn't return,
 *         the new program starts
 */
long sys_execve(const char *path, char * const argp[], char * const envp[])
{
    syscall_log(LOG_DEBUG, "execve(path=%p, argp=%p, envp=%p)\n", path, argp, envp);

    char *fn;
    int err = upath_get(path, &fn);

    if (err) {
        return err;
    }

    if (!*fn) {
        kfree(fn);
        return -ENOENT;
    }

    err = proc_execve(curthread, fn, argp, envp);

    kfree(fn);

    if (err)
        return err;

    /* start the new program, from its entry point: no return from here */
    sched_thread_spawn(curthread);
    return 0;
}

/**
 * Implements `fork()`: makes a child process, a copy of the caller.
 *
 * @return the child's process ID in the parent (the child gets 0), or a
 *         negative errno: `-ENOMEM`, `-EAGAIN`
 */
long sys_fork(void)
{
    syscall_log(LOG_DEBUG, "fork()\n");

    struct proc *fork = NULL;
    int err = proc_fork(curthread, &fork);

    if (err) {
        return err;
    }

    /* the child runs from fork()'s return, where it gets 0
     * (proc_fork()) */
    struct thread *thread = (struct thread *) fork->threads.head->value;
    sched_thread_ready(thread);

    return fork->pid;
}

/**
 * Implements `getpid()`.
 *
 * @return the calling process's ID
 */
long sys_getpid(void)
{
    syscall_log(LOG_DEBUG, "getpid()\n");
    return curproc->pid;
}

/**
 * Implements `sbrk()`: grows or shrinks the heap. The heap may grow up to
 * the next mapping (or the stack).
 *
 * @param incr  how many bytes to add (or take off, if negative)
 * @return the old end of the heap, or `(uintptr_t) -1` if the heap can't
 *         change that much
 */
long sys_sbrk(ptrdiff_t incr)
{
    syscall_log(LOG_DEBUG, "sbrk(incr=0x%x)\n", incr);

    uintptr_t heap_start = curproc->heap_start;
    uintptr_t heap = curproc->heap;

    /* the heap may grow up to the next mapped region (or the stack) */
    uintptr_t heap_limit = USER_STACK_BASE;
    struct qnode *next = curproc->heap_vm->qnode->next;

    if (next)
        heap_limit = ((struct vm_entry *) next->value)->base;

    if ((incr < 0 && (uintptr_t) -incr > heap - heap_start) ||
        (incr > 0 && (heap_limit < heap || (uintptr_t) incr > heap_limit - heap))) {
        /* the C library passes the result through and only treats
         * (void *) -1 as a failure, so return that instead of -ENOMEM */
        return (uintptr_t) -1;
    }

    curproc->heap = heap + incr;
    curproc->heap_vm->size = PAGE_ROUND(heap + incr - heap_start);

    return heap;
}

/**
 * Implements `times()`: the CPU times of the process and of its children
 * that it waited for, in ticks (`AQUILA_CLK_TCK` a second).
 *
 * @param buf  where to put them
 * @return the real time since boot, in ticks, or `-EFAULT`
 */
long sys_times(struct aquila_tms *buf)
{
    syscall_log(LOG_DEBUG, "times(buf=%p)\n", buf);

    const uint64_t tick = 1000000000ULL / AQUILA_CLK_TCK;   /* ns */

    struct aquila_tms tms = {
        .tms_utime  = curproc->utime / tick,
        .tms_stime  = curproc->stime / tick,
        .tms_cutime = curproc->cutime / tick,
        .tms_cstime = curproc->cstime / tick,
    };

    if (copyout(buf, &tms, sizeof(tms))) {
        return -EFAULT;
    }

    /* the real time elapsed since boot */
    return (uint32_t) (arch_rtime_ns() / tick);
}

/**
 * Implements `waitpid()`: waits for a child to end (or stop, or continue,
 * if asked), and reaps it if it ended.
 *
 * @param pid       which child: that one if > 0, any if -1, any in the
 *                  caller's process group if 0, or any in process group
 *                  `-pid` if < -1
 * @param stat_loc  where to put its status, or NULL
 * @param options   `WNOHANG`, `WUNTRACED`, `WCONTINUED`
 * @return the child's process ID, 0 if `WNOHANG` and none is ready, or a
 *         negative errno: `-ECHILD`, `-EINTR`, `-EFAULT`
 */
long sys_waitpid(int pid, int *stat_loc, int options)
{
    syscall_log(LOG_DEBUG, "waitpid(pid=%d, stat_loc=%p, options=0x%x)\n", pid, stat_loc, options);

    /* pid > 0:   that child
     * pid == -1: any child
     * pid == 0:  any child in the caller's process group
     * pid < -1:  any child in the process group -pid */
    pid_t pgid = pid == 0 ? curproc->pgrp->pgid : -pid;

    for (;;) {
        int found = 0;

        queue_for (node, procs) {
            struct proc *proc = node->value;
            int status, event = 0;

            if (proc->parent != curproc)
                continue;

            if (pid > 0 ? proc->pid != pid : pid != -1 && proc->pgrp->pgid != pgid)
                continue;

            found = 1;

            if (proc->zombie) {
                status = proc->exit;
            } else if ((options & WUNTRACED) && (proc->wait_event & WAIT_STOPPED)) {
                event = WAIT_STOPPED;
                status = (proc->stopped << 8) | 0x7f;
            } else if ((options & WCONTINUED) && (proc->wait_event & WAIT_CONTINUED)) {
                event = WAIT_CONTINUED;
                status = 0xffff;
            } else {
                continue;
            }

            /* if the status can't be written out, nothing is consumed: a
             * zombie, or a stop or continue event, stays to be waited for
             * again */
            if (stat_loc && copyout(stat_loc, &status, sizeof(status))) {
                return -EFAULT;
            }

            proc->wait_event &= ~event;

            pid_t child = proc->pid;

            if (proc->zombie)
                proc_reap(proc);

            return child;
        }

        if (!found) {
            return -ECHILD;
        }

        if (options & WNOHANG) {
            return 0;
        }

        if (waitq_sleep(&curproc->wait_queue)) {
            return -ERESTARTSYS;
        }
    }
}

/* the host name: set by sethostname(), shown by uname() */
static char nodename[sizeof(((struct utsname *) 0)->nodename)] = UTSNAME_NODENAME;

/**
 * Implements `uname()`: the system's name, release, version and machine,
 * and the host name.
 *
 * @param name  where to put them
 * @return 0, or `-EFAULT`
 */
long sys_uname(struct utsname *name)
{
    syscall_log(LOG_DEBUG, "uname(name=%p)\n", name);

    struct utsname u;
    memset(&u, 0, sizeof(u));

    strcpy(u.sysname,  UTSNAME_SYSNAME);
    strcpy(u.nodename, nodename);
    strcpy(u.release,  UTSNAME_RELEASE);
    /* the compiler's version string may be long */
    size_t n = MIN(sizeof(u.version) - 1, (size_t) strlen(UTSNAME_VERSION));
    memcpy(u.version, UTSNAME_VERSION, n);
    strcpy(u.machine,  UTSNAME_MACHINE);

    return copyout(name, &u, sizeof(u));
}

/**
 * Makes a new thread in the calling process (for `pthread_create()`), and
 * makes it ready to run.
 *
 * The new thread starts with the caller's signal mask, and nothing
 * pending.
 *
 * @param __uthread  the arguments, in user memory: the thread's stack,
 *                   the C library's start, the thread's function and its
 *                   argument, and the int to clear once it's gone
 * @return the new thread's ID, or a negative errno: `-EFAULT`, `-EINVAL`
 *         (it would start past user space), `-ENOMEM`
 */
long sys_thread_create(struct aquila_thread_args *__uthread)
{
    struct aquila_thread_args a;

    if (copyin(&a, __uthread, sizeof(a))) {
        return -EFAULT;
    }

    syscall_log(LOG_DEBUG,
            "thread_create(stack=%p, entry=%p, uentry=%p, arg=%p, attr=%p)\n",
            a.stack, a.entry, a.uentry, a.arg, a.attr);

    uintptr_t stack = a.stack;

    /* the kernel pushes the initial frame (3 words) on the user stack */
    if (stack < 3 * sizeof(uintptr_t) ||
        uaccess_check((void *) (stack - 3 * sizeof(uintptr_t)), 3 * sizeof(uintptr_t))) {
        return -EFAULT;
    }

    /* it starts in user space: on x86_64 returning to an address past it
     * (not canonical) would fault in the kernel */
    if (a.entry >= UACCESS_LIMIT)
        return -EINVAL;

    if (a.alive && uaccess_check((void *) a.alive, sizeof(int))) {
        return -EFAULT;
    }

    int err = 0;
    struct thread *thread = NULL;

    if ((err = thread_create(curthread, stack, a.entry, a.uentry, a.arg, a.attr, &thread))) {
        return err;
    }

    thread->alive = a.alive;

    sched_thread_ready(thread);
    return thread->tid;
}

/**
 * Ends the calling thread (for `pthread_exit()`). It stays as a zombie
 * until another thread joins it, unless it is detached: then it is freed
 * at once. The last live thread ending ends the process. Its `alive` int,
 * if it has one, is set to 0: it no longer uses its stack. It doesn't
 * return.
 *
 * @param value_ptr  the value that `thread_join()` returns
 * @return nothing: it doesn't return
 */
long sys_thread_exit(void *value_ptr)
{
    syscall_log(LOG_DEBUG, "thread_exit(value_ptr=%p)\n", value_ptr);

    curthread->exit_value = value_ptr;
    struct proc *owner = curproc;

    /* the last live thread ending ends the process */
    int alive = 0;
    queue_for (node, &owner->threads) {
        struct thread *thread = node->value;
        if (thread != curthread && thread->state != ZOMBIE)
            ++alive;
    }

    if (!alive)
        sys_exit(0);

    /* it's off its user stack: the C library may free it (a failed write
     * is the program's business) */
    if (curthread->alive) {
        int zero = 0;
        copyout((void *) curthread->alive, &zero, sizeof(zero));
    }

    /* wake up the threads waiting to join one */
    waitq_wakeup(&owner->thread_join);

    /* A detached thread leaves its process now, and is freed with its
     * kernel stack. Otherwise it stays on the list as a zombie, freed by
     * thread_join() or when the process ends. The kernel stack, which this
     * runs on, is freed from the idle stack by arch_cur_thread_kill(). */
    if (curthread->detached)
        queue_remove(&owner->threads, curthread);
    else
        curthread->state = ZOMBIE;

    arch_cur_thread_kill();

    /* not reached: the thread is gone */
    for (;;);
    return 0;
}

/**
 * Waits for a thread of the calling process to end, and frees it (for
 * `pthread_join()`).
 *
 * @param tid        the thread's ID
 * @param value_ptr  where to put the value it ended with, or NULL
 * @return the thread's ID, or a negative errno: `-ESRCH` (no such thread,
 *         or joined already), `-EDEADLK` (the caller itself), `-EINTR`,
 *         `-EFAULT`
 */
long sys_thread_join(int tid, void **value_ptr)
{
    syscall_log(LOG_DEBUG, "thread_join(tid=%d, value_ptr=%p)\n", tid, value_ptr);

    struct proc *owner = curproc;
    struct thread *thread;

    /* it's looked up again after each wait: another thread may have
     * joined or detached it meanwhile */
    for (;;) {
        thread = thread_find(owner, tid);

        /* no such thread, or it was joined already */
        if (!thread) {
            return -ESRCH;
        }

        /* a thread joining itself would wait forever */
        if (thread == curthread) {
            return -EDEADLK;
        }

        /* no one joins a detached thread */
        if (thread->detached) {
            return -EINVAL;
        }

        if (thread->state == ZOMBIE)
            break;

        if (waitq_sleep(&owner->thread_join)) {
            return -ERESTARTSYS;
        }
    }

    if (value_ptr && copyout(value_ptr, &thread->exit_value, sizeof(void *))) {
        return -EFAULT;
    }

    /* free what is left of it: thread_exit() freed the rest, and its CPU
     * is done with it, since it let go of the kernel lock */
    int joined = thread->tid;
    queue_remove(&owner->threads, thread);
    kfree(thread);

    return joined;
}

/**
 * Detaches a thread of the calling process (for `pthread_detach()`): no
 * one will join it, and it is freed as soon as it exits, or now if it
 * has already.
 *
 * @param tid  the thread's ID
 * @return 0, or a negative errno: `-ESRCH` (no such thread, or joined
 *         already), `-EINVAL` (detached already)
 */
long sys_thread_detach(int tid)
{
    syscall_log(LOG_DEBUG, "thread_detach(tid=%d)\n", tid);

    struct proc *owner = curproc;
    struct thread *thread = thread_find(owner, tid);

    if (!thread) {
        return -ESRCH;
    }

    if (thread->detached) {
        return -EINVAL;
    }

    thread->detached = 1;

    /* gone already: free what is left of it, as thread_join() would; the
     * threads waiting to join it fail with EINVAL */
    if (thread->state == ZOMBIE) {
        queue_remove(&owner->threads, thread);
        kfree(thread);
    }

    waitq_wakeup(&owner->thread_join);
    return 0;
}

/**
 * Implements `setpgid()`: moves a process (the caller or one of its
 * children) into a process group of its session.
 *
 * @param pid   the process, or 0 for the caller
 * @param pgid  the group's ID, or 0 for the process's own ID (a new group)
 * @return 0, or a negative errno: `-EINVAL`, `-ESRCH`, `-EPERM`,
 *         `-ENOMEM`
 */
long sys_setpgid(pid_t pid, pid_t pgid)
{
    syscall_log(LOG_DEBUG, "setpgid(pid=%d, pgid=%d)\n", pid, pgid);

    if (pgid < 0) {
        return -EINVAL;
    }

    /* only the caller or one of its children */
    struct proc *proc = pid ? proc_pid_find(pid) : curproc;

    if (!proc || (proc != curproc && proc->parent != curproc)) {
        return -ESRCH;
    }

    struct session *session = proc->pgrp->session;

    /* a session leader can't change its group, nor can a process outside
     * the caller's session */
    if (session != curproc->pgrp->session || session->leader == proc) {
        return -EPERM;
    }

    return proc_setpgid(proc, pgid ? pgid : proc->pid);
}

/**
 * Implements `setuid()`: sets the process's user ID. There are no separate
 * effective or saved IDs: root may set any ID, other users only their own.
 *
 * @param uid  the user ID
 * @return 0, or `-EPERM`
 */
long sys_setuid(uid_t uid)
{
    syscall_log(LOG_DEBUG, "setuid(uid=%d)\n", uid);

    if (curproc->uid != 0 && uid != curproc->uid) {
        return -EPERM;
    }

    curproc->uid = uid;
    return 0;
}

/**
 * Implements `setgid()`: sets the process's group ID. Root may set any ID,
 * other users only their own.
 *
 * @param gid  the group ID
 * @return 0, or `-EPERM`
 */
long sys_setgid(gid_t gid)
{
    syscall_log(LOG_DEBUG, "setgid(gid=%d)\n", gid);

    if (curproc->uid != 0 && gid != curproc->gid) {
        return -EPERM;
    }

    curproc->gid = gid;
    return 0;
}

/**
 * Implements `getuid()`.
 *
 * @return the calling process's user ID
 */
long sys_getuid(void)
{
    syscall_log(LOG_DEBUG, "getuid()\n");
    return curproc->uid;
}

/**
 * Implements `getgid()`.
 *
 * @return the calling process's group ID
 */
long sys_getgid(void)
{
    syscall_log(LOG_DEBUG, "getgid()\n");
    return curproc->gid;
}

/**
 * Implements `mmap()`: maps anonymous memory or a file into the calling
 * process. Pages are brought in by page faults, when first touched.
 *
 * @param uargs  the arguments, in user memory: the address, length,
 *               protection, flags, descriptor and offset
 * @param ret    where to put the mapping's address
 * @return 0, or a negative errno: `-EBADF`, `-EINVAL` (for an offset
 *         not on a page too), `-ENODEV`, `-ENOMEM`, `-EFAULT`
 */
long sys_mmap(struct aquila_mmap_args *uargs, void **ret)
{
    struct aquila_mmap_args _args, *args = &_args;
    void *none = NULL;

    /* the address is written out at the end: check first that it can be */
    if (copyin(args, uargs, sizeof(*args)) || copyout(ret, &none, sizeof(none))) {
        return -EFAULT;
    }

    syscall_log(LOG_DEBUG, "mmap(addr=%p, len=%d, prot=%x, flags=%x, fildes=%d, off=%d, ret=%p)\n",
            args->addr, args->len, args->prot, args->flags, args->fildes, args->off, ret);

    int err = 0;
    int anon = args->flags & MAP_ANONYMOUS;

    /* anonymous memory is the process's own (fork() copies it), so it
     * can't be shared */
    if (anon && (args->flags & MAP_SHARED)) {
        return -EINVAL;
    }

    struct file *file = anon ? NULL : fd_file(args->fildes);

    if (!anon && !file) {    /* the descriptor isn't open */
        return -EBADF;
    }

    if (file && (file->flags & FILE_SOCKET)) {
        return -ENODEV;
    }

    if (!args->len) {
        return -EINVAL;
    }

    /* a file's offset is on a page; a negative one would reach before
     * the file, which a pager may not expect */
    if (!anon && (args->off < 0 || (args->off & PAGE_MASK))) {
        return -EINVAL;
    }

    /* a fixed mapping must start on a page, and lie entirely in user
     * space */
    if ((args->flags & MAP_FIXED) && (((uintptr_t) args->addr & PAGE_MASK) ||
                uaccess_check(args->addr, args->len))) {
        return -EINVAL;
    }

    /* the mapping */
    struct vm_entry *vm_entry;
    vm_entry = vm_entry_new();

    if (vm_entry == NULL) {
        err = -ENOMEM;
        goto error;
    }

    vm_entry->base   = (uintptr_t) args->addr;
    vm_entry->size   = PAGE_ROUND(args->len);
    vm_entry->flags  = args->prot & PROT_READ  ? VM_UR : 0;
    vm_entry->flags |= args->prot & PROT_WRITE ? VM_UW : 0;
    vm_entry->flags |= args->prot & PROT_EXEC  ? VM_UX : 0;
    vm_entry->flags |= args->flags & MAP_SHARED ? VM_SHARED : 0;
    vm_entry->off    = anon ? 0 : args->off;

    /* without an object, pages are zero-filled when first touched */
    vm_entry->vm_object = anon ? NULL : vm_object_vnode(file->vnode);

    if (!anon && !vm_entry->vm_object) {
        err = -ENOMEM;
        goto error;
    }

    if (vm_entry->vm_object)
        vm_object_incref(vm_entry->vm_object);

    if (!(args->flags & MAP_FIXED))
        vm_entry->base = 0;  /* vm_space_insert() chooses the address */

    struct vm_space *vm_space = &curproc->vm_space;
    if ((err = vm_space_insert(vm_space, vm_entry)))
        goto error;

    if (!anon && !(args->flags & MAP_PRIVATE) && (err = vfs_map(vm_space, vm_entry)))
        goto error;

    void *addr = (void *) vm_entry->base;
    err = copyout(ret, &addr, sizeof(addr));

    return err;

error:
    if (vm_entry) {
        if (vm_entry->qnode)
            queue_node_remove(&curproc->vm_space.vm_entries, vm_entry->qnode);

        if (vm_entry->vm_object)
            vm_object_decref(vm_entry->vm_object);

        kfree(vm_entry);
    }

    return err;
}

/** Says whether a range of the current process takes in part of its
 * heap or its stack, or, with `whole`, any of either: `sbrk()` and
 * `exec()` manage them, and they can't be split. */
static int heap_stack_cut(vaddr_t start, vaddr_t end, int whole)
{
    struct vm_entry *vm[] = {curproc->heap_vm, curproc->stack_vm};

    for (size_t i = 0; i < sizeof(vm) / sizeof(vm[0]); ++i) {
        vaddr_t sva = vm[i]->base, eva = sva + vm[i]->size;

        /* not in the range */
        if (sva == eva || eva <= start || sva >= end)
            continue;

        if (whole || sva < start || eva > end)
            return 1;
    }

    return 0;
}

/**
 * Implements `munmap()`: removes the mappings of a range, whole mappings
 * or parts of them. Parts of the range that aren't mapped are left alone.
 * The heap and the stack can't be unmapped.
 *
 * @param addr  the range's start, on a page
 * @param len   its length
 * @return 0, or a negative errno: `-EINVAL` if the range doesn't start on
 *         a page, is empty, isn't in user space, or takes in some of the
 *         heap or the stack; `-ENOMEM` if there was no memory to split a
 *         mapping
 */
long sys_munmap(void *addr, size_t len)
{
    syscall_log(LOG_DEBUG, "munmap(addr=%p, len=%d)\n", addr, len);

    vaddr_t start = (vaddr_t) addr;

    if (!len || (start & PAGE_MASK) || uaccess_check(addr, len)) {
        return -EINVAL;
    }

    vaddr_t end = start + PAGE_ROUND(len);

    if (heap_stack_cut(start, end, 1)) {
        return -EINVAL;
    }

    return vm_space_remove(&curproc->vm_space, start, end);
}

/**
 * Implements `mprotect()`: changes the permissions of a range of
 * mappings, whole mappings or parts of them. The heap and the stack can
 * only be changed whole: they can't be split.
 *
 * @param addr  the range's start, on a page
 * @param len   its length (0 changes nothing)
 * @param prot  `PROT_NONE`, or `PROT_READ`, `PROT_WRITE` and `PROT_EXEC`
 * @return 0, or a negative errno: `-EINVAL` if the range doesn't start on
 *         a page, for another flag, or for part of the heap or the stack;
 *         `-ENOMEM` if a page of the range isn't mapped, or if there was no
 *         memory to split a mapping
 */
long sys_mprotect(void *addr, size_t len, int prot)
{
    syscall_log(LOG_DEBUG, "mprotect(addr=%p, len=%d, prot=%x)\n", addr, len, prot);

    vaddr_t start = (vaddr_t) addr;

    if ((start & PAGE_MASK) || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    /* past user space, nothing is the process's */
    if (uaccess_check(addr, len)) {
        return -ENOMEM;
    }

    vaddr_t end = start + PAGE_ROUND(len);

    if (heap_stack_cut(start, end, 0)) {
        return -EINVAL;
    }

    uint32_t perm = (prot & PROT_READ  ? VM_UR : 0) |
                    (prot & PROT_WRITE ? VM_UW : 0) |
                    (prot & PROT_EXEC  ? VM_UX : 0);

    return vm_space_protect(&curproc->vm_space, start, end, perm);
}

/**
 * Implements `sched_yield()`: lets the threads that are ready to run go
 * first; the caller goes on after them.
 *
 * @return 0
 */
long sys_sched_yield(void)
{
    syscall_log(LOG_DEBUG, "sched_yield()\n");
    sched_yield();
    return 0;
}

/**
 * Implements `sysconf()`: not supported; the C library answers it.
 *
 * @param name  the variable
 * @return `-ENOSYS`
 */
long sys_sysconf(int name)
{
    syscall_log(LOG_DEBUG, "sysconf(name=%d)\n", name);
    return -ENOSYS;
}

/**
 * Implements `getppid()`.
 *
 * @return the parent's process ID, or 0 if there is none
 */
long sys_getppid(void)
{
    syscall_log(LOG_DEBUG, "getppid()\n");

    /* orphans are init's; init and the kernel's process have no parent */
    return curproc->parent ? curproc->parent->pid : 0;
}

/**
 * Implements `getpgrp()`.
 *
 * @return the calling process's process group ID
 */
long sys_getpgrp(void)
{
    syscall_log(LOG_DEBUG, "getpgrp()\n");
    return curproc->pgrp->pgid;
}

/**
 * Implements `getrandom()` (and the C library's `getentropy()`): fills a
 * buffer with random bytes.
 *
 * @param buf    the buffer
 * @param len    its size
 * @param flags  not used
 * @return how many bytes were written, or `-EFAULT`
 */
long sys_getrandom(void *buf, size_t len, unsigned flags __unused)
{
    if (uaccess_check(buf, len)) {
        return -EFAULT;
    }

    /* a piece at a time, through a buffer on the stack */
    uint8_t kbuf[256];
    size_t done = 0;

    while (done < len) {
        size_t n = MIN(len - done, sizeof(kbuf));
        random_get(kbuf, n);

        if (copyout((char *) buf + done, kbuf, n))
            break;

        done += n;
    }

    memset(kbuf, 0, sizeof(kbuf));
    return done || !len ? (ssize_t) done : -EFAULT;
}

/**
 * Returns the calling thread's ID, for `pthread_self()`.
 *
 * @return the thread's ID
 */
long sys_thread_self(void)
{
    return curthread->tid;
}

/**
 * Implements `reboot()`: restarts, halts or powers off the machine, or
 * sets what Ctrl+Alt+Del does. Only root may.
 *
 * @param cmd  the command (`RB_*`)
 * @return 0 for the Ctrl+Alt+Del commands, `-EPERM`, or `-EINVAL`; the
 *         others don't return
 */
long sys_reboot(unsigned cmd)
{
    syscall_log(LOG_DEBUG, "reboot(cmd=0x%x)\n", cmd);

    if (curproc->uid) {
        return -EPERM;
    }

    return kernel_reboot(cmd);
}

/**
 * Implements `setsid()`: makes the calling process the leader of a new
 * session and process group.
 *
 * @return the new session's ID (the caller's process ID), or a negative
 *         errno: `-EPERM` for a process group leader, `-ENOMEM`
 */
long sys_setsid(void)
{
    syscall_log(LOG_DEBUG, "setsid()\n");

    struct proc *proc = curproc;
    struct pgroup *pgrp = proc->pgrp;
    struct qnode *pgrp_node = proc->pgrp_node;

    /* a group leader can't leave its group: the group would be in two
     * sessions */
    if (pgrp->pgid == proc->pid) {
        return -EPERM;
    }

    int ret = session_new(proc);

    if (ret) {
        proc->pgrp = pgrp;
        proc->pgrp_node = pgrp_node;
        return ret;
    }

    queue_node_remove(pgrp->procs, pgrp_node);
    pgrp_release(pgrp);
    return proc->pid;
}

/**
 * Implements `sethostname()`. Only root may.
 *
 * @param name  the host name (not NUL-terminated)
 * @param len   its length
 * @return 0, or a negative errno: `-EPERM`, `-EINVAL` if it's too long,
 *         `-EFAULT`
 */
long sys_sethostname(const char *name, size_t len)
{
    syscall_log(LOG_DEBUG, "sethostname(name=%p, len=%d)\n", name, len);

    if (curproc->uid != 0) {
        return -EPERM;
    }

    if (len >= sizeof(nodename)) {
        return -EINVAL;
    }

    char name_in[sizeof(nodename)];

    if (copyin(name_in, name, len)) {
        return -EFAULT;
    }

    memcpy(nodename, name_in, len);
    nodename[len] = '\0';
    return 0;
}
