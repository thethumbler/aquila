#ifndef _AQUILA_SYSCALL_H
#define _AQUILA_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/*
 * System call numbers.
 *
 * i386: number in %eax, arguments in %ebx, %ecx, %edx, %esi, %edi,
 * `int $0x80', result (or negated errno) in %eax.
 *
 * x86_64: number in %rax, arguments in %rdi, %rsi, %rdx, %r10, %r8,
 * `int $0x80', result (or negated errno) in %rax.
 *
 * aarch64: number in x8, arguments in x0 to x4, `svc #0', result (or
 * negated errno) in x0.
 */

#define SYS_exit            1
#define SYS_close           2
#define SYS_execve          3
#define SYS_fork            4
#define SYS_fstat           5
#define SYS_getpid          6
#define SYS_isatty          7
#define SYS_kill            8
#define SYS_link            9
#define SYS_lseek           10
#define SYS_open            11
#define SYS_read            12
#define SYS_sbrk            13
#define SYS_stat            14
#define SYS_times           15
#define SYS_unlink          16
#define SYS_waitpid         17
#define SYS_write           18
#define SYS_ioctl           19
#define SYS_sigaction       20
#define SYS_readdir         21
#define SYS_mount           22
#define SYS_mkdir           23
#define SYS_uname           24
#define SYS_pipe            25
#define SYS_fcntl           26
#define SYS_chdir           27
#define SYS_getcwd          28
#define SYS_thread_create   29
#define SYS_thread_exit     30
#define SYS_thread_join     31
#define SYS_setpgid         32
#define SYS_mknod           33
#define SYS_lstat           34
/* 35 was auth, removed: login checks /etc/shadow and uses setuid() */
#define SYS_getuid          36
#define SYS_getgid          37
#define SYS_mmap            38
#define SYS_munmap          39
#define SYS_socket          40
#define SYS_accept          41
#define SYS_bind            42
#define SYS_connect         43
#define SYS_listen          44
#define SYS_send            45
#define SYS_recv            46
#define SYS_umask           47
#define SYS_chmod           48
#define SYS_sysconf         49
#define SYS_gettimeofday    50
#define SYS_access          51
#define SYS_sigmask         52
#define SYS_select          53
#define SYS_getpgrp         54
#define SYS_chown           55
#define SYS_fchown          56
#define SYS_lchown          57
/* 58 was utime, replaced by utimensat */
#define SYS_rmdir           59
#define SYS_setuid          60
#define SYS_setgid          61
#define SYS_ftruncate       62
#define SYS_sigsuspend      63
#define SYS_getppid         64
#define SYS_openat          65
#define SYS_fstatat         66
#define SYS_unlinkat        67
#define SYS_mknodat         68
#define SYS_linkat          69
#define SYS_symlinkat       70
#define SYS_readlinkat      71
#define SYS_renameat        72
#define SYS_faccessat       73
#define SYS_fchmodat        74
#define SYS_fchownat        75
#define SYS_utimensat       76
#define SYS_fchmod          77
#define SYS_fchdir          78
#define SYS_setsid          79
#define SYS_nanosleep       80
#define SYS_clock_gettime   81
#define SYS_sethostname     82
#define SYS_poll            83
#define SYS_flock           84
#define SYS_reboot          85
#define SYS_sync            86
#define SYS_shutdown        87
#define SYS_setitimer       88
#define SYS_thread_self     89
#define SYS_getrandom       90
#define SYS_getsockname     91
#define SYS_getpeername     92
#define SYS_getsockopt      93
#define SYS_setsockopt      94
#define SYS_sigreturn       95  /* from the kernel's sigcode, a handler's return */

#define SYS_MAX             95

/*
 * Arguments of the older system calls that take more than three, passed
 * as a pointer to one of these structures.
 */

/* SYS_mount */
struct aquila_mount_args {
    const char *type;
    const char *dir;
    int         flags;
    void       *data;
} __attribute__((packed));

/* SYS_mmap, the mapped address is returned through the second argument */
struct aquila_mmap_args {
    void   *addr;
    size_t  len;
    int     prot;
    int     flags;
    int     fildes;
    long    off;
} __attribute__((packed));

/* SYS_send, SYS_recv: sendto() and recvfrom() too, the address (and its
 * length) of where to send, or of where it came from; NULL if none */
struct aquila_socket_io_args {
    int     fd;
    void   *buf;
    size_t  len;
    int     flags;
    void   *addr;
    unsigned int *addrlen;
} __attribute__((packed));

/* SYS_select, also pselect(): the signal mask while waiting, if sigmask */
struct aquila_select_args {
    int     nfds;
    void   *readfds;
    void   *writefds;
    void   *exceptfds;
    void   *timeout;
    void   *sigmask;
};

/* SYS_thread_create */
struct aquila_thread_args {
    uintptr_t stack;
    uintptr_t entry;    /* entry point as seen by the kernel */
    uintptr_t uentry;   /* user function */
    uintptr_t arg;
    uintptr_t attr;
};

#endif /* ! _AQUILA_SYSCALL_H */
