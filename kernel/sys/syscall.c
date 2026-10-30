/** \file
 * System calls: what the architecture calls when user mode makes one
 * (`syscall_dispatch()`), the table of them, and the helpers that several
 * kinds of calls share, for the user memory and descriptors they take.
 * The calls themselves are in `sys_file.c`, `sys_proc.c`, `sys_signal.c`,
 * `sys_net.c` and `sys_poll.c`, and `syscalls.h` lists them all.
 */

#include "syscalls.h"

static int syscall_log_level = LOG_NONE;
LOGGER_DEFINE(syscall, syscall_log, syscall_log_level)

/**
 * Finds the open file that a descriptor of the current process refers to,
 * and holds it until the system call returns, so that a `close()` by
 * another thread meanwhile doesn't free it.
 *
 * @param fd  the descriptor
 * @return the open file, or NULL if `fd` isn't open
 */
struct file *fd_file(int fd)
{
    struct file *file = proc_fd_file(curproc, fd);
    return file ? file_hold(file) : NULL;
}

/*
 * User memory: a system call reads what the program passed it with
 * copyin(), or copyinstr() for a string (upath_get() for a path), and
 * writes its results with copyout(); data goes through an iobuf
 * (fs/iobuf.h). A bad address makes them fail with EFAULT. Past them, the
 * kernel only works with its own copies.
 */

/**
 * Copies a path from user memory into a new kernel buffer.
 *
 * @param upath  the path, in user memory
 * @param path   set to the kernel's copy, which the caller frees with
 *               `kfree()`; NULL on failure
 * @return 0, `-ENOMEM` if there was no memory for the copy, `-EFAULT` if
 *         `upath` is a bad address, or `-ENAMETOOLONG` if the path is
 *         longer than `UPATH_MAX`
 */
int upath_get(const char *upath, char **path)
{
    char *buf = kmalloc(UPATH_MAX, &M_BUFFER, 0);

    *path = NULL;

    if (!buf)
        return -ENOMEM;

    long len = copyinstr(buf, upath, UPATH_MAX);

    if (len < 0) {
        kfree(buf);
        return len;
    }

    *path = buf;
    return 0;
}

/*
 * The table says what each system call number calls. All its entries
 * have the one type that the dispatch calls them through, syscall_t: five
 * arguments the size of a register, and a long result. A system call is
 * written with its own types instead, as sys_read(int fd, void *buf,
 * size_t nbytes).
 *
 * SYSCALLn(name, types...) makes the table's entry for sys_<name>, which
 * takes n arguments: a function sys_<name>_entry() of the table's type,
 * which converts the registers to those types and calls sys_<name>(). So
 * the compiler checks every call. Calling sys_read() itself through a
 * pointer of another type would be undefined behaviour in C, even though
 * x86's and aarch64's calling conventions happen to make it work.
 */

#define SYSCALL_ARGS \
    uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5

#define SYSCALL0(name) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name(); }
#define SYSCALL1(name, t1) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name((t1) a1); }
#define SYSCALL2(name, t1, t2) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name((t1) a1, (t2) a2); }
#define SYSCALL3(name, t1, t2, t3) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name((t1) a1, (t2) a2, (t3) a3); }
#define SYSCALL4(name, t1, t2, t3, t4) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name((t1) a1, (t2) a2, (t3) a3, (t4) a4); }
#define SYSCALL5(name, t1, t2, t3, t4, t5) \
    static long sys_##name##_entry(SYSCALL_ARGS) \
    { return sys_##name((t1) a1, (t2) a2, (t3) a3, (t4) a4, (t5) a5); }

SYSCALL1(exit, int)
SYSCALL1(close, int)
SYSCALL3(execve, const char *, char * const *, char * const *)
SYSCALL0(fork)
SYSCALL2(fstat, int, struct aquila_stat *)
SYSCALL0(getpid)
SYSCALL1(isatty, int)
SYSCALL2(kill, pid_t, int)
SYSCALL2(link, const char *, const char *)
SYSCALL5(linkat, int, const char *, int, const char *, int)
SYSCALL3(lseek, int, off_t, int)
SYSCALL3(open, const char *, int, mode_t)
SYSCALL4(openat, int, const char *, int, mode_t)
SYSCALL3(read, int, void *, size_t)
SYSCALL1(sbrk, ptrdiff_t)
SYSCALL2(stat, const char *, struct aquila_stat *)
SYSCALL4(fstatat, int, const char *, struct aquila_stat *, int)
SYSCALL1(times, struct aquila_tms *)
SYSCALL1(unlink, const char *)
SYSCALL3(unlinkat, int, const char *, int)
SYSCALL3(waitpid, int, int *, int)
SYSCALL3(write, int, void *, size_t)
SYSCALL3(ioctl, int, int, void *)
SYSCALL3(sigaction, int, const struct aquila_sigaction *, struct aquila_sigaction *)
SYSCALL2(readdir, int, struct dirent *)
SYSCALL1(mount, struct aquila_mount_args *)
SYSCALL2(mkdir, const char *, mode_t)
SYSCALL1(uname, struct utsname *)
SYSCALL1(pipe, int *)
SYSCALL3(fcntl, int, int, uintptr_t)
SYSCALL1(chdir, const char *)
SYSCALL2(getcwd, char *, size_t)
SYSCALL1(thread_create, struct aquila_thread_args *)
SYSCALL1(thread_exit, void *)
SYSCALL2(thread_join, int, void **)
SYSCALL2(setpgid, pid_t, pid_t)
SYSCALL3(mknod, const char *, uint32_t, uint32_t)
SYSCALL4(mknodat, int, const char *, uint32_t, uint32_t)
SYSCALL2(lstat, const char *, struct aquila_stat *)
SYSCALL1(setuid, uid_t)
SYSCALL2(ftruncate, int, off_t)
SYSCALL1(setgid, gid_t)
SYSCALL0(getuid)
SYSCALL0(getgid)
SYSCALL2(mmap, struct aquila_mmap_args *, void **)
SYSCALL2(munmap, void *, size_t)
SYSCALL3(socket, int, int, int)
SYSCALL3(accept, int, const struct sockaddr *, socklen_t *)
SYSCALL3(bind, int, const struct sockaddr *, socklen_t)
SYSCALL3(connect, int, const struct sockaddr *, socklen_t)
SYSCALL2(listen, int, int)
SYSCALL1(send, struct aquila_socket_io_args *)
SYSCALL1(recv, struct aquila_socket_io_args *)
SYSCALL3(getsockname, int, struct sockaddr *, socklen_t *)
SYSCALL3(getpeername, int, struct sockaddr *, socklen_t *)
SYSCALL5(getsockopt, int, int, int, void *, socklen_t *)
SYSCALL5(setsockopt, int, int, int, const void *, socklen_t)
SYSCALL1(umask, mode_t)
SYSCALL2(chmod, const char *, mode_t)
SYSCALL4(fchmodat, int, const char *, mode_t, int)
SYSCALL2(fchmod, int, mode_t)
SYSCALL1(sysconf, int)
SYSCALL2(access, const char *, int)
SYSCALL4(faccessat, int, const char *, int, int)
SYSCALL2(gettimeofday, struct timeval *, struct timezone *)
SYSCALL3(sigmask, int, const sigset_t *, sigset_t *)
SYSCALL1(sigsuspend, const sigset_t *)
SYSCALL0(sigreturn)
SYSCALL2(thread_kill, int, int)
SYSCALL1(thread_detach, int)
SYSCALL3(sigtimedwait, const sigset_t *, struct aquila_siginfo *, const struct aquila_timespec *)
SYSCALL1(sigpending, sigset_t *)
SYSCALL0(getppid)
SYSCALL1(select, struct aquila_select_args *)
SYSCALL3(poll, struct pollfd *, nfds_t, int)
SYSCALL0(getpgrp)
SYSCALL3(chown, const char *, uid_t, gid_t)
SYSCALL5(fchownat, int, const char *, uid_t, gid_t, int)
SYSCALL3(fchown, int, uid_t, gid_t)
SYSCALL3(lchown, const char *, uid_t, gid_t)
SYSCALL4(utimensat, int, const char *, const struct aquila_timespec *, int)
SYSCALL1(rmdir, const char *)
SYSCALL3(symlinkat, const char *, int, const char *)
SYSCALL4(readlinkat, int, const char *, char *, size_t)
SYSCALL4(renameat, int, const char *, int, const char *)
SYSCALL2(flock, int, int)
SYSCALL0(sync)
SYSCALL2(shutdown, int, int)
SYSCALL3(setitimer, int, const struct aquila_itimer *, struct aquila_itimer *)
SYSCALL3(getrandom, void *, size_t, unsigned)
SYSCALL0(thread_self)
SYSCALL1(reboot, unsigned)
SYSCALL1(fchdir, int)
SYSCALL0(setsid)
SYSCALL2(nanosleep, const struct aquila_timespec *, struct aquila_timespec *)
SYSCALL2(clock_gettime, int, struct aquila_timespec *)
SYSCALL2(sethostname, const char *, size_t)

const syscall_t syscall_table[] = {
    [SYS_exit]          = sys_exit_entry,
    [SYS_close]         = sys_close_entry,
    [SYS_execve]        = sys_execve_entry,
    [SYS_fork]          = sys_fork_entry,
    [SYS_fstat]         = sys_fstat_entry,
    [SYS_getpid]        = sys_getpid_entry,
    [SYS_isatty]        = sys_isatty_entry,
    [SYS_kill]          = sys_kill_entry,
    [SYS_link]          = sys_link_entry,
    [SYS_lseek]         = sys_lseek_entry,
    [SYS_open]          = sys_open_entry,
    [SYS_read]          = sys_read_entry,
    [SYS_sbrk]          = sys_sbrk_entry,
    [SYS_stat]          = sys_stat_entry,
    [SYS_times]         = sys_times_entry,
    [SYS_unlink]        = sys_unlink_entry,
    [SYS_waitpid]       = sys_waitpid_entry,
    [SYS_write]         = sys_write_entry,
    [SYS_ioctl]         = sys_ioctl_entry,
    [SYS_sigaction]     = sys_sigaction_entry,
    [SYS_readdir]       = sys_readdir_entry,
    [SYS_mount]         = sys_mount_entry,
    [SYS_mkdir]         = sys_mkdir_entry,
    [SYS_uname]         = sys_uname_entry,
    [SYS_pipe]          = sys_pipe_entry,
    [SYS_fcntl]         = sys_fcntl_entry,
    [SYS_chdir]         = sys_chdir_entry,
    [SYS_getcwd]        = sys_getcwd_entry,
    [SYS_thread_create] = sys_thread_create_entry,
    [SYS_thread_exit]   = sys_thread_exit_entry,
    [SYS_thread_join]   = sys_thread_join_entry,
    [SYS_setpgid]       = sys_setpgid_entry,
    [SYS_mknod]         = sys_mknod_entry,
    [SYS_lstat]         = sys_lstat_entry,
    [SYS_getuid]        = sys_getuid_entry,
    [SYS_getgid]        = sys_getgid_entry,
    [SYS_mmap]          = sys_mmap_entry,
    [SYS_munmap]        = sys_munmap_entry,
    [SYS_socket]        = sys_socket_entry,
    [SYS_accept]        = sys_accept_entry,
    [SYS_bind]          = sys_bind_entry,
    [SYS_connect]       = sys_connect_entry,
    [SYS_listen]        = sys_listen_entry,
    [SYS_send]          = sys_send_entry,
    [SYS_recv]          = sys_recv_entry,
    [SYS_umask]         = sys_umask_entry,
    [SYS_chmod]         = sys_chmod_entry,
    [SYS_sysconf]       = sys_sysconf_entry,
    [SYS_gettimeofday]  = sys_gettimeofday_entry,
    [SYS_access]        = sys_access_entry,
    [SYS_sigmask]       = sys_sigmask_entry,
    [SYS_select]        = sys_select_entry,
    [SYS_getpgrp]       = sys_getpgrp_entry,
    [SYS_chown]         = sys_chown_entry,
    [SYS_fchown]        = sys_fchown_entry,
    [SYS_lchown]        = sys_lchown_entry,
    [SYS_rmdir]         = sys_rmdir_entry,
    [SYS_setuid]        = sys_setuid_entry,
    [SYS_setgid]        = sys_setgid_entry,
    [SYS_ftruncate]     = sys_ftruncate_entry,
    [SYS_sigsuspend]    = sys_sigsuspend_entry,
    [SYS_getppid]       = sys_getppid_entry,
    [SYS_openat]        = sys_openat_entry,
    [SYS_fstatat]       = sys_fstatat_entry,
    [SYS_unlinkat]      = sys_unlinkat_entry,
    [SYS_mknodat]       = sys_mknodat_entry,
    [SYS_linkat]        = sys_linkat_entry,
    [SYS_symlinkat]     = sys_symlinkat_entry,
    [SYS_readlinkat]    = sys_readlinkat_entry,
    [SYS_renameat]      = sys_renameat_entry,
    [SYS_faccessat]     = sys_faccessat_entry,
    [SYS_fchmodat]      = sys_fchmodat_entry,
    [SYS_fchownat]      = sys_fchownat_entry,
    [SYS_utimensat]     = sys_utimensat_entry,
    [SYS_fchmod]        = sys_fchmod_entry,
    [SYS_fchdir]        = sys_fchdir_entry,
    [SYS_setsid]        = sys_setsid_entry,
    [SYS_nanosleep]     = sys_nanosleep_entry,
    [SYS_clock_gettime] = sys_clock_gettime_entry,
    [SYS_sethostname]   = sys_sethostname_entry,
    [SYS_poll]          = sys_poll_entry,
    [SYS_flock]         = sys_flock_entry,
    [SYS_reboot]        = sys_reboot_entry,
    [SYS_sync]          = sys_sync_entry,
    [SYS_shutdown]      = sys_shutdown_entry,
    [SYS_setitimer]     = sys_setitimer_entry,
    [SYS_thread_self]   = sys_thread_self_entry,
    [SYS_getrandom]     = sys_getrandom_entry,
    [SYS_getsockname]   = sys_getsockname_entry,
    [SYS_getpeername]   = sys_getpeername_entry,
    [SYS_getsockopt]    = sys_getsockopt_entry,
    [SYS_setsockopt]    = sys_setsockopt_entry,
    [SYS_sigreturn]     = sys_sigreturn_entry,
    [SYS_thread_kill]   = sys_thread_kill_entry,
    [SYS_thread_detach] = sys_thread_detach_entry,
    [SYS_sigtimedwait]  = sys_sigtimedwait_entry,
    [SYS_sigpending]    = sys_sigpending_entry,
};

const size_t syscall_cnt = sizeof(syscall_table)/sizeof(syscall_table[0]);


/* how many "undefined syscall" messages a second at most: any process can
 * make such calls in a loop, and would flood the console */
#define NOSYS_PER_SEC   5

/** Reports a call to an undefined system call on the console, at most
 * `NOSYS_PER_SEC` times a second; the ones left out are counted, and the
 * next message says how many there were. */
static void nosys_report(unsigned long nr)
{
    static uint64_t second;     /* the second the messages are counted in */
    static unsigned shown;      /* the messages shown in that second */
    static unsigned dropped;    /* the ones left out since the last shown */

    uint64_t now = arch_rtime_ns() / 1000000000;

    if (now != second) {
        second = now;
        shown  = 0;
    }

    if (shown == NOSYS_PER_SEC) {
        dropped++;
        return;
    }

    shown++;

    if (dropped) {
        printk("(%u undefined syscalls not shown)\n", dropped);
        dropped = 0;
    }

    printk("[%d:%d] %s: undefined syscall %ld\n", curproc->pid, curthread->tid,
           curproc->name, (int64_t) nr);
}

/**
 * Runs a system call that the current thread made from user mode. The
 * architecture's `arch_syscall()` calls it with the number and the
 * arguments, as they came in registers.
 *
 * The call's result is set as the thread's result to return to user mode
 * (`arch_syscall_return()`), except for `sigreturn`'s: its registers are
 * the ones it restored from the signal frame. A call that doesn't return
 * (`exit`, or an `execve` that worked) doesn't come back here.
 *
 * @param nr  the system call's number (`SYS_*`)
 * @param a1  its first argument
 * @param a2  its second argument
 * @param a3  its third argument
 * @param a4  its fourth argument
 * @param a5  its fifth argument
 */
void syscall_dispatch(unsigned long nr, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                      uintptr_t a4, uintptr_t a5)
{
    curthread->syscall_nr = nr;

    if (nr >= syscall_cnt || !syscall_table[nr]) {
        nosys_report(nr);
        arch_syscall_return(curthread, -ENOSYS);
        return;
    }

    long ret = syscall_table[nr](a1, a2, a3, a4, a5);

    if (nr != SYS_sigreturn)
        arch_syscall_return(curthread, ret);

    /* the open files the call used: let go of */
    thread_files_release(curthread);

    /* a restart was this call, or none */
    curthread->restarting = 0;
}
