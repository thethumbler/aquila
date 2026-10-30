/*
 * AquilaOS system call wrappers
 *
 * Built with MISSING_SYSCALL_NAMES, so newlib's reentrant layer calls
 * these directly (read, write, ...) instead of _read, _write, ...
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <aquila/syscall.h>
#include <aquila/time.h>

#include "syscall.h"

void _exit(int status)
{
    __syscall(SYS_exit, status, 0, 0);

    /* not reached */
    for (;;);
}

int close(int fildes)
{
    return SYSCALL(SYS_close, fildes, 0, 0);
}

/* newlib's posix/ provides execve() and the exec* family on top of this */
int _execve(const char *path, char *const argv[], char *const envp[])
{
    return SYSCALL(SYS_execve, path, argv, envp);
}

pid_t fork(void)
{
    return SYSCALL(SYS_fork, 0, 0, 0);
}

pid_t vfork(void)
{
    return fork();
}

int fstat(int fildes, struct stat *buf)
{
    return SYSCALL(SYS_fstat, fildes, buf, 0);
}

int stat(const char *path, struct stat *buf)
{
    return SYSCALL(SYS_stat, path, buf, 0);
}

int lstat(const char *path, struct stat *buf)
{
    return SYSCALL(SYS_lstat, path, buf, 0);
}

pid_t getpid(void)
{
    return SYSCALL(SYS_getpid, 0, 0, 0);
}

pid_t getppid(void)
{
    return SYSCALL(SYS_getppid, 0, 0, 0);
}

/* newlib's posix/ provides isatty() on top of this: a terminal is what
 * answers terminal ioctls */
int _isatty(int fildes)
{
    struct termios tios;

    if (tcgetattr(fildes, &tios)) {
        if (errno != EBADF)
            errno = ENOTTY;
        return 0;
    }

    return 1;
}

int kill(pid_t pid, int sig)
{
    return SYSCALL(SYS_kill, pid, sig, 0);
}

int killpg(pid_t pgrp, int sig)
{
    if (pgrp < 0) {
        errno = EINVAL;
        return -1;
    }

    return kill(-pgrp, sig);
}

int link(const char *old, const char *new)
{
    return SYSCALL(SYS_link, old, new, 0);
}

int unlink(const char *path)
{
    return SYSCALL(SYS_unlink, path, 0, 0);
}

off_t lseek(int fildes, off_t offset, int whence)
{
    return SYSCALL(SYS_lseek, fildes, offset, whence);
}

ssize_t pread(int fildes, void *buf, size_t nbytes, off_t offset)
{
    return SYSCALL5(SYS_pread, fildes, buf, nbytes, offset, 0);
}

ssize_t pwrite(int fildes, const void *buf, size_t nbytes, off_t offset)
{
    return SYSCALL5(SYS_pwrite, fildes, buf, nbytes, offset, 0);
}

int open(const char *path, int oflags, ...)
{
    mode_t mode = 0;

    if (oflags & O_CREAT) {
        va_list ap;
        va_start(ap, oflags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    return SYSCALL(SYS_open, path, oflags, mode);
}

ssize_t read(int fildes, void *buf, size_t nbytes)
{
    return SYSCALL(SYS_read, fildes, buf, nbytes);
}

ssize_t write(int fildes, const void *buf, size_t nbytes)
{
    return SYSCALL(SYS_write, fildes, buf, nbytes);
}

void *sbrk(ptrdiff_t incr)
{
    /* the kernel returns the old break, or (void *) -1 */
    void *ret = (void *) __syscall(SYS_sbrk, incr, 0, 0);

    if (ret == (void *) -1)
        errno = ENOMEM;

    return ret;
}

clock_t times(struct tms *buf)
{
    /* the result is a time, any value is valid */
    long ret = __syscall(SYS_times, (long) buf, 0, 0);

    if (ret < 0 && ret >= -4095) {
        errno = -ret;
        return (clock_t) -1;
    }

    return ret;
}

pid_t waitpid(pid_t pid, int *stat_loc, int options)
{
    return SYSCALL(SYS_waitpid, pid, stat_loc, options);
}

pid_t wait(int *stat_loc)
{
    return waitpid(-1, stat_loc, 0);
}

/* BSD wait3(), the kernel doesn't track resource usage */
pid_t wait3(int *stat_loc, int options, struct rusage *rusage)
{
    if (rusage)
        memset(rusage, 0, sizeof(*rusage));

    return waitpid(-1, stat_loc, options);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    return SYSCALL(SYS_sigaction, sig, act, oact);
}

_sig_func_ptr signal(int sig, _sig_func_ptr handler)
{
    struct sigaction act = {0}, oact = {0};

    act.sa_handler = handler;

    if (sigaction(sig, &act, &oact) == -1)
        return SIG_ERR;

    return oact.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return SYSCALL(SYS_sigmask, how, set, oldset);
}

int sigaltstack(const stack_t *ss, stack_t *oss)
{
    return SYSCALL(SYS_sigaltstack, ss, oss, 0);
}

int sigpending(sigset_t *set)
{
    return SYSCALL(SYS_sigpending, set, 0, 0);
}

/* the signal taken, without running its handler; EAGAIN once the time is
 * up */
int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
    return SYSCALL(SYS_sigtimedwait, set, info, timeout);
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info)
{
    return sigtimedwait(set, info, NULL);
}

/* the error number, not -1 and errno, as the pthread functions */
int sigwait(const sigset_t *set, int *sig)
{
    long ret;

    /* it isn't ended by another signal's handler (POSIX: no EINTR) */
    while ((ret = __syscall(SYS_sigtimedwait, (long) set, 0, 0)) == -EINTR)
        ;

    if (ret < 0)
        return -ret;

    *sig = ret;
    return 0;
}

int sigsuspend(const sigset_t *mask)
{
    /* always fails with EINTR once a signal handler ran */
    return SYSCALL(SYS_sigsuspend, mask, 0, 0);
}

/* until a signal is caught, with the signal mask as it is */
int pause(void)
{
    sigset_t mask;

    sigprocmask(SIG_BLOCK, NULL, &mask);
    return sigsuspend(&mask);
}

int ioctl(int fildes, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);

    return SYSCALL(SYS_ioctl, fildes, request, argp);
}

int mount(const char *type, const char *dir, int flags, void *data)
{
    struct aquila_mount_args args = {type, dir, flags, data};
    return SYSCALL(SYS_mount, &args, 0, 0);
}

int mkdir(const char *path, mode_t mode)
{
    return SYSCALL(SYS_mkdir, path, mode, 0);
}

int rmdir(const char *path)
{
    return SYSCALL(SYS_rmdir, path, 0, 0);
}

int gethostname(char *name, size_t len)
{
    struct utsname uts;

    if (uname(&uts))
        return -1;

    if (strlen(uts.nodename) >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(name, uts.nodename);
    return 0;
}

int sethostname(const char *name, size_t len)
{
    return SYSCALL(SYS_sethostname, name, len, 0);
}

int uname(struct utsname *name)
{
    return SYSCALL(SYS_uname, name, 0, 0);
}

int pipe(int fd[2])
{
    return SYSCALL(SYS_pipe, fd, 0, 0);
}

int pipe2(int fd[2], int flags)
{
    return SYSCALL(SYS_pipe2, fd, flags, 0);
}

int fcntl(int fildes, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);

    return SYSCALL(SYS_fcntl, fildes, cmd, arg);
}

int dup(int fildes)
{
    return fcntl(fildes, F_DUPFD, 0);
}

int dup2(int fildes, int fildes2)
{
    /* dup3() refuses the same descriptor twice; dup2() checks it's open */
    if (fildes == fildes2)
        return fcntl(fildes, F_GETFD) == -1 ? -1 : fildes2;

    return SYSCALL(SYS_dup3, fildes, fildes2, 0);
}

int dup3(int fildes, int fildes2, int flags)
{
    return SYSCALL(SYS_dup3, fildes, fildes2, flags);
}

int fchdir(int fildes)
{
    return SYSCALL(SYS_fchdir, fildes, 0, 0);
}

int chdir(const char *path)
{
    return SYSCALL(SYS_chdir, path, 0, 0);
}

char *getcwd(char *buf, size_t size)
{
    if (SYSCALL(SYS_getcwd, buf, size, 0) == -1)
        return NULL;

    return buf;
}

int setpgid(pid_t pid, pid_t pgid)
{
    return SYSCALL(SYS_setpgid, pid, pgid, 0);
}

/* System V's: a group of its own */
int setpgrp(void)
{
    return setpgid(0, 0);
}

pid_t setsid(void)
{
    return SYSCALL(SYS_setsid, 0, 0, 0);
}

pid_t getpgrp(void)
{
    return SYSCALL(SYS_getpgrp, 0, 0, 0);
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
    return SYSCALL(SYS_mknod, path, mode, dev);
}

uid_t getuid(void)
{
    return __syscall(SYS_getuid, 0, 0, 0);
}

uid_t geteuid(void)
{
    return getuid();
}

gid_t getgid(void)
{
    return __syscall(SYS_getgid, 0, 0, 0);
}

gid_t getegid(void)
{
    return getgid();
}

int getgroups(int size, gid_t list[])
{
    /* no supplementary groups */
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int setgroups(int size, const gid_t *list)
{
    /* there are no supplementary groups to set, a process only has its
     * group id, as if the list were empty */
    if (getuid()) {
        errno = EPERM;
        return -1;
    }

    return 0;
}

int getrlimit(int resource, struct rlimit *rlp)
{
    if (resource < 0 || resource >= RLIM_NLIMITS) {
        errno = EINVAL;
        return -1;
    }

    /* only the number of descriptors is limited */
    rlp->rlim_cur = rlp->rlim_max = resource == RLIMIT_NOFILE ? OPEN_MAX : RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlp)
{
    if (resource < 0 || resource >= RLIM_NLIMITS) {
        errno = EINVAL;
        return -1;
    }

    /* the kernel can't change or enforce limits */
    errno = ENOSYS;
    return -1;
}

int nice(int incr)
{
    /* processes have no priorities */
    errno = ENOSYS;
    return -1;
}

int chroot(const char *path)
{
    /* processes have no root directory of their own */
    errno = ENOSYS;
    return -1;
}

/* there are no separate effective ids */
int setuid(uid_t uid)
{
    return SYSCALL(SYS_setuid, uid, 0, 0);
}

int seteuid(uid_t uid)
{
    return setuid(uid);
}

int setgid(gid_t gid)
{
    return SYSCALL(SYS_setgid, gid, 0, 0);
}

int setegid(gid_t gid)
{
    return setgid(gid);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off)
{
    struct aquila_mmap_args args = {addr, len, prot, flags, fildes, off};
    void *ret;

    if (SYSCALL(SYS_mmap, &args, &ret, 0) == -1)
        return MAP_FAILED;

    return ret;
}

int munmap(void *addr, size_t len)
{
    return SYSCALL(SYS_munmap, addr, len, 0);
}

int mprotect(void *addr, size_t len, int prot)
{
    return SYSCALL(SYS_mprotect, addr, len, prot);
}

int msync(void *addr, size_t len, int flags)
{
    return SYSCALL(SYS_msync, addr, len, flags);
}

int sched_yield(void)
{
    return SYSCALL(SYS_sched_yield, 0, 0, 0);
}

/* SOCK_NONBLOCK and SOCK_CLOEXEC on a new descriptor */
static int sock_flags(int fd, int flags)
{
    if (fd < 0)
        return fd;

    if (flags & SOCK_NONBLOCK)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    if (flags & SOCK_CLOEXEC)
        fcntl(fd, F_SETFD, FD_CLOEXEC);

    return fd;
}

int socket(int domain, int type, int protocol)
{
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    return sock_flags(SYSCALL(SYS_socket, domain, type & ~flags, protocol), flags);
}

/* the kernel takes SOCK_NONBLOCK and SOCK_CLOEXEC itself here */
int socketpair(int domain, int type, int protocol, int fd[2])
{
    return SYSCALL5(SYS_socketpair, domain, type, protocol, fd, 0);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return SYSCALL(SYS_accept, fd, addr, len);
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }

    return sock_flags(accept(fd, addr, len), flags);
}

/* sendmsg() and recvmsg(): one buffer for the pieces, through sendto()
 * and recvfrom(); no ancillary data */
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    if (msg->msg_controllen) {
        errno = EOPNOTSUPP;
        return -1;
    }

    size_t len = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i)
        len += msg->msg_iov[i].iov_len;

    char *buf = malloc(len ? len : 1);
    if (!buf)
        return -1;

    size_t off = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        memcpy(buf + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        off += msg->msg_iov[i].iov_len;
    }

    ssize_t r = sendto(fd, buf, len, flags, msg->msg_name, msg->msg_namelen);
    free(buf);
    return r;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    size_t len = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i)
        len += msg->msg_iov[i].iov_len;

    char *buf = malloc(len ? len : 1);
    if (!buf)
        return -1;

    socklen_t namelen = msg->msg_namelen;
    ssize_t r = recvfrom(fd, buf, len, flags, msg->msg_name, msg->msg_name ? &namelen : NULL);

    if (r >= 0) {
        size_t off = 0;
        for (int i = 0; i < msg->msg_iovlen && off < (size_t) r; ++i) {
            size_t n = msg->msg_iov[i].iov_len < (size_t) r - off ? msg->msg_iov[i].iov_len : (size_t) r - off;
            memcpy(msg->msg_iov[i].iov_base, buf + off, n);
            off += n;
        }

        msg->msg_namelen = namelen;
        msg->msg_controllen = 0;
        msg->msg_flags = 0;
    }

    free(buf);
    return r;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return SYSCALL(SYS_bind, fd, addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return SYSCALL(SYS_connect, fd, addr, len);
}

int listen(int fd, int backlog)
{
    return SYSCALL(SYS_listen, fd, backlog, 0);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen)
{
    struct aquila_socket_io_args args = {fd, (void *) buf, len, flags, (void *) addr, &addrlen};
    return SYSCALL(SYS_send, &args, 0, 0);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen)
{
    struct aquila_socket_io_args args = {fd, buf, len, flags, addr, addr ? addrlen : NULL};
    return SYSCALL(SYS_recv, &args, 0, 0);
}

int shutdown(int fd, int how)
{
    return SYSCALL(SYS_shutdown, fd, how, 0);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return SYSCALL(SYS_getsockname, fd, addr, len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return SYSCALL(SYS_getpeername, fd, addr, len);
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    return SYSCALL5(SYS_getsockopt, fd, level, name, val, len);
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    return SYSCALL5(SYS_setsockopt, fd, level, name, val, len);
}

mode_t umask(mode_t mask)
{
    return __syscall(SYS_umask, mask, 0, 0);
}

int chmod(const char *path, mode_t mode)
{
    return SYSCALL(SYS_chmod, path, mode, 0);
}

int fchmod(int fildes, mode_t mode)
{
    return SYSCALL(SYS_fchmod, fildes, mode, 0);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    return SYSCALL(SYS_chown, path, owner, group);
}

int fchown(int fildes, uid_t owner, gid_t group)
{
    return SYSCALL(SYS_fchown, fildes, owner, group);
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    return SYSCALL(SYS_lchown, path, owner, group);
}

int access(const char *path, int mode)
{
    return SYSCALL(SYS_access, path, mode, 0);
}

long sysconf(int name)
{
    switch (name) {
        case _SC_ARG_MAX:           return ARG_MAX;     /* the kernel takes 128K */
        case _SC_CHILD_MAX:         return CHILD_MAX;
        case _SC_CLK_TCK:           return AQUILA_CLK_TCK;
        case _SC_NGROUPS_MAX:       return NGROUPS_MAX;
        case _SC_OPEN_MAX:          return 64;          /* FDS_COUNT */
        case _SC_PAGESIZE:          return 4096;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:  return 1;
        case _SC_HOST_NAME_MAX:     return 63;          /* utsname nodename */
        case _SC_LOGIN_NAME_MAX:    return 32;
        case _SC_TTY_NAME_MAX:      return 32;
        case _SC_SYMLOOP_MAX:       return SYMLOOP_MAX;
        case _SC_LINE_MAX:          return LINE_MAX;
        case _SC_RE_DUP_MAX:        return RE_DUP_MAX;
        case _SC_EXPR_NEST_MAX:     return EXPR_NEST_MAX;
        case _SC_BC_BASE_MAX:       return BC_BASE_MAX;
        case _SC_BC_DIM_MAX:        return BC_DIM_MAX;
        case _SC_BC_SCALE_MAX:      return BC_SCALE_MAX;
        case _SC_BC_STRING_MAX:     return BC_STRING_MAX;
        case _SC_COLL_WEIGHTS_MAX:  return COLL_WEIGHTS_MAX;
        case _SC_IOV_MAX:           return IOV_MAX;
        case _SC_GETPW_R_SIZE_MAX:
        case _SC_GETGR_R_SIZE_MAX:  return 1024;
        case _SC_JOB_CONTROL:       return 1;
        case _SC_MONOTONIC_CLOCK:   return _POSIX_MONOTONIC_CLOCK;
    }

    errno = EINVAL;
    return -1;
}

/* the same for every file */
static long file_limit(int name)
{
    switch (name) {
        case _PC_LINK_MAX:          return LINK_MAX;
        case _PC_MAX_CANON:         return MAX_CANON;
        case _PC_MAX_INPUT:         return MAX_INPUT;
        case _PC_NAME_MAX:          return MAXNAMELEN - 1;
        case _PC_PATH_MAX:          return PATH_MAX;
        case _PC_PIPE_BUF:          return PIPE_BUF;
        case _PC_SYMLINK_MAX:       return 1023;        /* lookup reads 1K */
        case _PC_CHOWN_RESTRICTED:  return 1;
        case _PC_NO_TRUNC:          return 1;
        case _PC_VDISABLE:          return 0;
    }

    errno = EINVAL;
    return -1;
}

long pathconf(const char *path, int name)
{
    struct stat st;

    if (stat(path, &st))
        return -1;

    return file_limit(name);
}

long fpathconf(int fildes, int name)
{
    struct stat st;

    if (fstat(fildes, &st))
        return -1;

    return file_limit(name);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return SYSCALL(SYS_poll, fds, nfds, timeout);
}

int flock(int fd, int operation)
{
    return SYSCALL(SYS_flock, fd, operation, 0);
}

int reboot(int cmd)
{
    return SYSCALL(SYS_reboot, cmd, 0, 0);
}

int gettimeofday(struct timeval *__restrict tv, void *__restrict tz)
{
    return SYSCALL(SYS_gettimeofday, tv, tz, 0);
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
        struct timeval *timeout)
{
    struct aquila_select_args args = {nfds, readfds, writefds, exceptfds, timeout, NULL};
    return SYSCALL(SYS_select, &args, 0, 0);
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
        const struct timespec *timeout, const sigset_t *sigmask)
{
    struct timeval tv;

    if (timeout) {
        if (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) {
            errno = EINVAL;
            return -1;
        }

        /* rounded up, waiting at least that long */
        tv.tv_sec  = timeout->tv_sec;
        tv.tv_usec = (timeout->tv_nsec + 999) / 1000;

        if (tv.tv_usec == 1000000) {
            tv.tv_sec++;
            tv.tv_usec = 0;
        }
    }

    /* the kernel swaps the signal mask for the wait */
    struct aquila_select_args args = {nfds, readfds, writefds, exceptfds,
        timeout ? &tv : NULL, (void *) sigmask};
    return SYSCALL(SYS_select, &args, 0, 0);
}

static uint64_t timeval_ns(const struct timeval *tv)
{
    return (uint64_t) tv->tv_sec * 1000000000ULL + (uint64_t) tv->tv_usec * 1000;
}

static void ns_timeval(uint64_t ns, struct timeval *tv)
{
    tv->tv_sec = ns / 1000000000ULL;
    tv->tv_usec = (ns % 1000000000ULL + 999) / 1000;

    if (tv->tv_usec == 1000000) {
        tv->tv_sec++;
        tv->tv_usec = 0;
    }
}

/* the real time timer only (ITIMER_REAL), the kernel counts no virtual
 * time */
int setitimer(int which, const struct itimerval *restrict value,
              struct itimerval *restrict ovalue)
{
    struct aquila_itimer new, old;

    if (value) {
        if (value->it_value.tv_usec < 0 || value->it_value.tv_usec >= 1000000 ||
            value->it_interval.tv_usec < 0 || value->it_interval.tv_usec >= 1000000 ||
            value->it_value.tv_sec < 0 || value->it_interval.tv_sec < 0) {
            errno = EINVAL;
            return -1;
        }

        new.value = timeval_ns(&value->it_value);
        new.interval = timeval_ns(&value->it_interval);
    }

    if (SYSCALL(SYS_setitimer, which, value ? &new : NULL, &old) < 0)
        return -1;

    if (ovalue) {
        ns_timeval(old.value, &ovalue->it_value);
        ns_timeval(old.interval, &ovalue->it_interval);
    }

    return 0;
}

int getitimer(int which, struct itimerval *value)
{
    return setitimer(which, NULL, value);
}

unsigned alarm(unsigned seconds)
{
    struct itimerval new = {{0, 0}, {seconds, 0}}, old;

    if (setitimer(ITIMER_REAL, &new, &old) < 0)
        return 0;

    /* seconds left of the previous one, rounded (0 only if there was none) */
    return old.it_value.tv_sec + (old.it_value.tv_usec >= 500000 ||
            (!old.it_value.tv_sec && old.it_value.tv_usec));
}

int getpagesize(void)
{
    return 4096;
}

int getdtablesize(void)
{
    return sysconf(_SC_OPEN_MAX);
}

void sync(void)
{
    /* writes reach the devices' write caches before write() returns, what
     * shared mappings wrote stays in the page cache: sync writes back both */
    SYSCALL(SYS_sync, 0, 0, 0);
}

int fsync(int fildes)
{
    return SYSCALL(SYS_fsync, fildes, 0, 0);
}

int fdatasync(int fildes)
{
    return fsync(fildes);
}

int issetugid(void)
{
    /* no set-user-ID or set-group-ID support */
    return 0;
}

char *getlogin(void)
{
    return getenv("LOGNAME");
}

int getentropy(void *buf, size_t len)
{
    /* at most 256 bytes, as everywhere */
    if (len > 256) {
        errno = EIO;
        return -1;
    }

    return SYSCALL(SYS_getrandom, buf, len, 0) < 0 ? -1 : 0;
}

int ftruncate(int fildes, off_t len)
{
    return SYSCALL(SYS_ftruncate, fildes, len, 0);
}

int truncate(const char *path, off_t len)
{
    int fd = open(path, O_WRONLY);

    if (fd == -1)
        return -1;

    int ret = ftruncate(fd, len);
    int err = errno;

    close(fd);
    errno = err;

    return ret;
}

/* POSIX names for newlib's line reading functions */
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
    return __getdelim(lineptr, n, delim, stream);
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    return __getline(lineptr, n, stream);
}
