#ifndef _SYS_SYSCALLS_H
#define _SYS_SYSCALLS_H

/*
 * Every system call's handler, sys_<name>(), with its own argument types
 * and a long result (a negative result is an errno), grouped by the file
 * it is in. sys/syscall.c's table calls them through SYSCALLn(). This
 * header is private to sys/; the handlers' comments are in their files.
 */

#include <core/system.h>
#include <aquila/syscall.h>
#include <core/panic.h>
#include <core/string.h>
#include <core/arch.h>
#include <core/time.h>

#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <sys/binfmt.h>
#include <sys/uaccess.h>
#include <sys/syscall.h>

#include <fs/iobuf.h>

#include <net/socket.h>

#include <aquila/errno.h>
#include <aquila/dirent.h>
#include <aquila/utsname.h>
#include <aquila/fcntl.h>
#include <aquila/mman.h>
#include <aquila/time.h>
#include <aquila/ioctl.h>
#include <aquila/wait.h>
#include <aquila/poll.h>
#include <aquila/un.h>

#include <fs/devpts.h>
#include <fs/pipe.h>
#include <fs/stat.h>

#include <mm/vm.h>
#include <sys/reboot.h>
#include <dev/random.h>


LOGGER_DECLARE(syscall_log)

/* syscall.c: what several kinds of calls share */
struct file *fd_file(int fd);
/* the longest path a call takes, with its NUL (as newlib's PATH_MAX) */
#define UPATH_MAX   1024
int upath_get(const char *upath, char **path);

/* sys_file.c: files, their names and attributes, directories, mounts and
 * locks */
long sys_close(int fildes);
long sys_fstat(int fildes, struct aquila_stat *buf);
long sys_isatty(int fildes);
long sys_link(const char *oldpath, const char *newpath);
long sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
long sys_lseek(int fildes, off_t offset, int whence);
long sys_open(const char *path, int oflags, mode_t mode);
long sys_openat(int dirfd, const char *path, int oflags, mode_t mode);
long sys_read(int fildes, void *buf, size_t nbytes);
long sys_stat(const char *path, struct aquila_stat *buf);
long sys_fstatat(int dirfd, const char *path, struct aquila_stat *buf, int flags);
long sys_unlink(const char *path);
long sys_unlinkat(int dirfd, const char *path, int flags);
long sys_write(int fd, void *buf, size_t nbytes);
long sys_ioctl(int fd, int request, void *argp);
long sys_readdir(int fd, struct dirent *dirent);
long sys_mount(struct aquila_mount_args *args);
long sys_mkdir(const char *path, mode_t mode);
long sys_pipe(int fd[2]);
long sys_fcntl(int fd, int cmd, uintptr_t arg);
long sys_chdir(const char *upath);
long sys_getcwd(char *buf, size_t size);
long sys_mknod(const char *path, uint32_t mode, uint32_t dev);
long sys_mknodat(int dirfd, const char *path, uint32_t mode, uint32_t dev);
long sys_lstat(const char *path, struct aquila_stat *buf);
long sys_ftruncate(int fildes, off_t len);
long sys_umask(mode_t mask);
long sys_chmod(const char *path, mode_t mode);
long sys_fchmodat(int dirfd, const char *path, mode_t mode, int flags);
long sys_fchmod(int fd, mode_t mode);
long sys_access(const char *path, int mode);
long sys_faccessat(int dirfd, const char *path, int mode, int flags);
long sys_chown(const char *path, uid_t owner, gid_t group);
long sys_fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags);
long sys_fchown(int fd, uid_t owner, gid_t group);
long sys_lchown(const char *path, uid_t owner, gid_t group);
long sys_utimensat(int dirfd, const char *path, const struct aquila_timespec times[2], int flags);
long sys_rmdir(const char *path);
long sys_symlinkat(const char *utarget, int dirfd, const char *upath);
long sys_readlinkat(int dirfd, const char *upath, char *buf, size_t size);
long sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
long sys_flock(int fd, int op);
long sys_sync(void);
long sys_fchdir(int fd);

/* sys_proc.c: processes and threads, their IDs and groups, memory, and
 * the machine */
long sys_exit(int code);
long sys_execve(const char *path, char * const argp[], char * const envp[]);
long sys_fork(void);
long sys_getpid(void);
long sys_sbrk(ptrdiff_t incr);
long sys_times(struct aquila_tms *buf);
long sys_waitpid(int pid, int *stat_loc, int options);
long sys_uname(struct utsname *name);
long sys_thread_create(struct aquila_thread_args *__uthread);
long sys_thread_exit(void *value_ptr);
long sys_thread_join(int tid, void **value_ptr);
long sys_thread_detach(int tid);
long sys_setpgid(pid_t pid, pid_t pgid);
long sys_setuid(uid_t uid);
long sys_setgid(gid_t gid);
long sys_getuid(void);
long sys_getgid(void);
long sys_mmap(struct aquila_mmap_args *uargs, void **ret);
long sys_munmap(void *addr, size_t len);
long sys_sysconf(int name);
long sys_getppid(void);
long sys_getpgrp(void);
long sys_getrandom(void *buf, size_t len, unsigned flags __unused);
long sys_thread_self(void);
long sys_reboot(unsigned cmd);
long sys_setsid(void);
long sys_sethostname(const char *name, size_t len);

/* sys_signal.c: signals, timers, sleeping and the clocks */
long sys_kill(pid_t pid, int sig);
long sys_thread_kill(int tid, int sig);
long sys_sigtimedwait(const sigset_t *set, struct aquila_siginfo *info,
                      const struct aquila_timespec *timeout);
long sys_sigaction(int sig, const struct aquila_sigaction *act, struct aquila_sigaction *oact);
long sys_gettimeofday(struct timeval *tv, struct timezone *tz);
long sys_sigmask(int how, const sigset_t *set, sigset_t *oldset);
long sys_sigsuspend(const sigset_t *set);
long sys_sigpending(sigset_t *set);
long sys_sigaltstack(const struct aquila_stack *ss, struct aquila_stack *oss);
long sys_sigreturn(void);
long sys_setitimer(int which, const struct aquila_itimer *new, struct aquila_itimer *old);
long sys_nanosleep(const struct aquila_timespec *req, struct aquila_timespec *rem);
long sys_clock_gettime(int clock, struct aquila_timespec *ts);

/* sys_net.c: sockets */
long sys_socket(int domain, int type, int protocol);
long sys_accept(int fd, const struct sockaddr *addr, socklen_t *len);
long sys_bind(int fd, const struct sockaddr *addr, socklen_t len);
long sys_connect(int fd, const struct sockaddr *addr, socklen_t len);
long sys_listen(int fd, int backlog);
long sys_send(struct aquila_socket_io_args *us);
long sys_recv(struct aquila_socket_io_args *us);
long sys_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
long sys_getpeername(int fd, struct sockaddr *addr, socklen_t *len);
long sys_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
long sys_setsockopt(int fd, int level, int name, const void *val, socklen_t len);
long sys_shutdown(int fd, int how);

/* sys_poll.c: select() and poll(), waiting for any of several descriptors */
long sys_select(struct aquila_select_args *args);
long sys_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* ! _SYS_SYSCALLS_H */
