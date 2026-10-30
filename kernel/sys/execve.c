/** \file
 * `execve()`: a process runs another program.
 *
 * The arguments and the environment are copied into the kernel first,
 * since the process's memory is about to go. A script ("#!" on its first
 * line) runs as its interpreter. Then the program is loaded
 * (`binfmt_load()`), the close-on-exec descriptors are closed, and caught
 * signals go back to their default action. Last, the program's initial
 * stack is built, the same on every architecture (`exec_stack_build()`),
 * and the architecture starts the thread at the program's entry point
 * (`arch_exec_start()`).
 */

#include <core/system.h>
#include <core/string.h>
#include <core/arch.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <mm/pmap.h>
#include <sys/binfmt.h>
#include <sys/uaccess.h>
#include <mm/mm.h>
#include <aquila/fcntl.h>
#include <aquila/auxv.h>

/* the most that the arguments and the environment may take, strings and
 * pointers together */
#define EXEC_ARG_MAX    (128 * 1024)

/** Frees an array of `count` strings that `exec_strings_get()` made. */
static void exec_strings_free(char **strs, int count)
{
    if (!strs)
        return;

    for (int i = 0; i < count; ++i)
        kfree(strs[i]);

    kfree(strs);
}

/**
 * Copies a NULL-terminated array of strings (the arguments or the
 * environment) from user memory into a NULL-terminated array of kernel
 * copies, setting `*count` to how many there are and `*ref` to the array.
 * Their size, strings and pointers, is added to `*total`, which may not
 * go past `EXEC_ARG_MAX`. Each string comes in through `buf`, of
 * `EXEC_ARG_MAX` bytes. Returns 0, or `-EFAULT`, `-E2BIG` or `-ENOMEM`.
 */
static int exec_strings_get(char * const u_strs[], char *buf, size_t *total, int *count, char ***ref)
{
    int cnt = 0, err = 0;
    char *u;

    /* count the strings */
    for (; u_strs; ++cnt) {
        if (copyin(&u, &u_strs[cnt], sizeof(u)))
            return -EFAULT;

        if (!u)
            break;

        if ((cnt + 1) * sizeof(char *) > EXEC_ARG_MAX)
            return -E2BIG;
    }

    char **strs = kmalloc((cnt + 1) * sizeof(char *), &M_BUFFER, M_ZERO);

    if (!strs)
        return -ENOMEM;

    for (int i = 0; i < cnt; ++i) {
        size_t room = EXEC_ARG_MAX - *total;
        long len;

        /* the array may have changed since it was counted */
        if (copyin(&u, &u_strs[i], sizeof(u)) || !u) {
            err = -EFAULT;
            goto error;
        }

        if (!room || (len = copyinstr(buf, u, room)) == -ENAMETOOLONG) {
            err = -E2BIG;
            goto error;
        }

        if (len < 0) {
            err = len;
            goto error;
        }

        *total += len + 1 + sizeof(char *);

        if (*total > EXEC_ARG_MAX) {
            err = -E2BIG;
            goto error;
        }

        if (!(strs[i] = kmalloc(len + 1, &M_BUFFER, 0))) {
            err = -ENOMEM;
            goto error;
        }

        memcpy(strs[i], buf, len + 1);
    }

    *count = cnt;
    *ref = strs;
    return 0;

error:
    exec_strings_free(strs, cnt);
    return err;
}

/* how many interpreters in a row may be scripts themselves */
#define EXEC_SCRIPT_DEPTH   4

/**
 * Reads the interpreter of a script, from "#!interpreter [argument]" on
 * its first line. Sets `*interp` to the interpreter and `*arg` to the
 * argument (the rest of the line), or to NULL if the file isn't a script
 * or there is no argument. Returns 0 or a negative errno.
 */
static int exec_script(struct proc *proc, const char *path, char **interp, char **arg)
{
    struct uio uio = PROC_UIO(proc);
    struct vnode *vnode = NULL;
    char buf[128];
    int err;

    *interp = *arg = NULL;

    if ((err = vfs_lookup(path, &uio, &vnode)))
        return err;

    /* like any program, it must be a regular file that the process may
     * execute */
    if (!S_ISREG(vnode->mode))
        err = -EACCES;
    else
        err = vfs_access(vnode, &uio, VFS_X);

    ssize_t len = err ? 0 : vfs_read(vnode, 0, sizeof(buf) - 1, buf);
    vfs_close(vnode);

    if (err)
        return err;

    if (len < 2 || buf[0] != '#' || buf[1] != '!')
        return 0;

    buf[len] = '\0';

    char *end = buf + 2;
    while (*end && *end != '\n')
        ++end;

    /* the whole line has to fit in the buffer */
    if (!*end)
        return -ENOEXEC;

    *end = '\0';

    char *p = buf + 2;
    while (*p == ' ' || *p == '\t')
        ++p;

    char *name = p;
    while (*p && *p != ' ' && *p != '\t')
        ++p;

    if (p == name)
        return -ENOEXEC;

    if (*p)
        *p++ = '\0';

    while (*p == ' ' || *p == '\t')
        ++p;

    /* trailing blanks aren't part of the argument */
    while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    if (!(*interp = strdup(name)))
        return -ENOMEM;

    if (*p && !(*arg = strdup(p))) {
        kfree(*interp);
        *interp = NULL;
        return -ENOMEM;
    }

    return 0;
}

/**
 * Makes the arguments of a script's interpreter: the interpreter, its
 * argument from the script if there is one, the script's path, then the
 * script's own arguments but the first (`argv[0]`). They replace `*argp`
 * and `*argc`. Returns 0 or `-ENOMEM`.
 */
static int exec_script_args(char *interp, char *arg, const char *path, char ***argp, int *argc)
{
    int count = 1 + !!arg + 1 + (*argc > 1 ? *argc - 1 : 0);
    char **args = kmalloc((count + 1) * sizeof(char *), &M_BUFFER, M_ZERO);
    char *script = strdup(path);

    if (!args || !script) {
        kfree(args);
        kfree(script);
        return -ENOMEM;
    }

    int i = 0;
    args[i++] = interp;
    if (arg)
        args[i++] = arg;
    args[i++] = script;

    for (int j = 1; j < *argc; ++j)
        args[i++] = (*argp)[j];

    if (*argc)
        kfree((*argp)[0]);
    kfree(*argp);

    *argp = args;
    *argc = count;
    return 0;
}

/**
 * Builds a new program's initial stack, at the top of its user stack, as
 * the System V ABI has it on every architecture (and as Linux and the
 * BSDs do). From the stack pointer up, one word each:
 *
 *      argc
 *      argv[0] ... argv[argc - 1], NULL
 *      envp[0] ... envp[envc - 1], NULL
 *      the auxiliary vector: (type, value) pairs, AT_NULL last
 *      ... then the strings, at the top of the stack
 *
 * The stack pointer is 16-byte aligned, as every ABI wants it at a
 * program's entry. The C library's start code (crt0) finds everything
 * from there. The process's address space must be the current one: its
 * stack's pages are written directly, and come in as they are touched.
 * Returns the stack pointer.
 */
static uintptr_t exec_stack_build(struct proc *proc, int argc, char * const argv[], int envc, char * const envp[])
{
    /* what the program is told about itself (aquila/auxv.h); there are
     * no separate effective IDs */
    const uintptr_t auxv[][2] = {
        {AT_PAGESZ, PAGE_SIZE},
        {AT_ENTRY,  proc->entry},
        {AT_UID,    proc->uid},
        {AT_EUID,   proc->uid},
        {AT_GID,    proc->gid},
        {AT_EGID,   proc->gid},
        {AT_NULL,   0},
    };
    const int nauxv = sizeof(auxv) / sizeof(auxv[0]);

    /* the strings go at the top; the caller bounds their total size
     * (EXEC_ARG_MAX), well below the stack's */
    size_t strings = 0;

    for (int i = 0; i < argc; ++i)
        strings += strlen(argv[i]) + 1;

    for (int i = 0; i < envc; ++i)
        strings += strlen(envp[i]) + 1;

    char *str = (char *) (USER_STACK - strings);

    /* the words below them: argc, the two arrays with their NULLs, and
     * the auxiliary vector's pairs */
    size_t words = 1 + (argc + 1) + (envc + 1) + 2 * nauxv;
    uintptr_t sp = ((uintptr_t) str - words * sizeof(uintptr_t)) & ~(uintptr_t) 15;
    uintptr_t *w = (uintptr_t *) sp;

    *w++ = argc;

    for (int i = 0; i < argc; ++i) {
        strcpy(str, argv[i]);
        *w++ = (uintptr_t) str;
        str += strlen(argv[i]) + 1;
    }

    *w++ = 0;

    for (int i = 0; i < envc; ++i) {
        strcpy(str, envp[i]);
        *w++ = (uintptr_t) str;
        str += strlen(envp[i]) + 1;
    }

    *w++ = 0;

    for (int i = 0; i < nauxv; ++i) {
        *w++ = auxv[i][0];
        *w++ = auxv[i][1];
    }

    return sp;
}

/** Starts a process's thread at its program's entry point, with its
 * initial stack built from `argv` and `envp` (`argc` and `envc` strings,
 * in kernel memory). The process's address space must be the current one. */
static void exec_start(struct proc *proc, int argc, char * const argv[], int envc, char * const envp[])
{
    struct thread *thread = (struct thread *) proc->threads.head->value;
    uintptr_t sp = exec_stack_build(proc, argc, argv, envc, envp);

    arch_exec_start(thread, proc->entry, sp);
}

/**
 * Sets up init, the first process, to start its program: its initial
 * stack with its arguments and environment, before it first runs. The
 * kernel calls it at boot, when no process runs yet.
 *
 * @param proc  init's process, with its program loaded
 * @param argc  how many arguments there are
 * @param argv  the arguments, in kernel memory
 * @param envc  how many environment strings there are
 * @param envp  the environment strings, in kernel memory
 */
void init_execve(struct proc *proc, int argc, char * const argv[], int envc, char * const envp[])
{
    /* init's address space is made current, and init's thread the
     * current thread, so that the page faults of the writes to its stack
     * bring its pages in */
    curthread = (struct thread *) proc->threads.head->value;
    pmap_switch(proc->vm_space.pmap);

    exec_start(proc, argc, argv, envc, envp);

    curthread = NULL;
}

/**
 * Runs another program in a thread's process, for `execve()`. On success,
 * the thread starts the new program when it returns to user mode.
 *
 * @param thread  the thread calling `execve()`
 * @param fn      the program's path
 * @param _argp   the arguments, in user memory
 * @param _envp   the environment, in user memory
 * @return 0, or a negative errno: `-EFAULT`, `-E2BIG`, `-ENOMEM`,
 *         `-ELOOP` (too many nested scripts), or the loader's. If loading
 *         fails after the old program is gone, the process is killed
 *         (`SIGSEGV`) instead.
 */
int proc_execve(struct thread *thread, const char *fn, char * const _argp[], char * const _envp[])
{
    int err = 0;
    struct proc *proc = thread->owner;

    int argc = 0, envc = 0;
    size_t total = 0;

    char **argp = NULL;
    char **envp = NULL;

    /* copy the arguments and the environment into the kernel */
    char *buf = kmalloc(EXEC_ARG_MAX, &M_BUFFER, 0);

    if (!buf)
        return -ENOMEM;

    if (!(err = exec_strings_get(_argp, buf, &total, &argc, &argp)))
        err = exec_strings_get(_envp, buf, &total, &envc, &envp);

    kfree(buf);

    if (err)
        goto free_resources;

    /* a script runs as its interpreter */
    for (int depth = 0;; ++depth) {
        char *interp, *arg;

        if ((err = exec_script(proc, fn, &interp, &arg)))
            goto free_resources;

        if (!interp)
            break;

        if (depth == EXEC_SCRIPT_DEPTH ||
            (err = exec_script_args(interp, arg, fn, &argp, &argc))) {
            kfree(interp);
            kfree(arg);
            err = err ? err : -ELOOP;
            goto free_resources;
        }

        fn = interp;
    }

    if ((err = binfmt_load(proc, fn, NULL))) {
        if (!proc->vm_space.vm_entries.count) {
            /* the failure came after the old program was destroyed:
             * there is nothing to return to, so the process is killed */
            exec_strings_free(argp, argc);
            exec_strings_free(envp, envc);

            proc->exit = PROC_EXIT(SIGSEGV, SIGSEGV);
            proc_kill(proc);
            arch_sleep();
        }

        goto free_resources;
    }

    thread->spawned = 0;

    /* close the descriptors marked close-on-exec */
    for (int fd = 0; fd < FDS_COUNT; ++fd)
        if (proc_fd_file(proc, fd) && (proc->fds[fd].flags & FD_CLOEXEC))
            proc_fd_close(proc, fd);

    /* caught signals go back to their default action, and ignored ones
     * stay ignored (POSIX); the signal mask and the pending signals stay,
     * the alternate stack, in the old program's memory, goes */
    for (int i = 0; i <= SIG_MAX; ++i) {
        if (proc->sigaction[i].sa_handler != SIG_IGN)
            proc->sigaction[i].sa_handler = SIG_DFL;

        proc->sigaction[i].sa_mask = 0;
        proc->sigaction[i].sa_flags = 0;
    }

    curthread->sig_altstack = (struct aquila_stack) {0};

    exec_start(proc, argc, argp, envc, envp);

free_resources:
    exec_strings_free(argp, argc);
    exec_strings_free(envp, envc);

    return err;
}
