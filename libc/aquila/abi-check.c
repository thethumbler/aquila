/*
 * Compile-time check that the C library agrees with the kernel ABI
 * (include/uapi), run by toolchain/build.sh with -Werror after libc is
 * installed. Nothing here ends up in libc.
 *
 * errno and signal numbers: newlib's headers are included first, a
 * different value in <aquila/...> is then a macro redefinition error.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>

#include <aquila/errno.h>

/* newlib's struct sigaction has sa_handler and sa_sigaction in a union,
 * the macros would rename the kernel's field */
#undef sa_handler
#undef sa_sigaction
#include <aquila/signal.h>
#include <aquila/ucontext.h>
#include <aquila/stat.h>
#include <aquila/time.h>
#include <aquila/wait.h>

#define SAME_FIELD(a, b, f) \
    _Static_assert(offsetof(struct a, f) == offsetof(struct b, f) && \
                   sizeof(((struct a *) 0)->f) == sizeof(((struct b *) 0)->f), \
                   "struct " #a " ." #f " does not match the kernel")

#define SAME_SIZE(a, b) \
    _Static_assert(sizeof(struct a) == sizeof(struct b), \
                   "struct " #a " size does not match the kernel")

SAME_SIZE(stat, aquila_stat);
SAME_FIELD(stat, aquila_stat, st_dev);
SAME_FIELD(stat, aquila_stat, st_ino);
SAME_FIELD(stat, aquila_stat, st_mode);
SAME_FIELD(stat, aquila_stat, st_nlink);
SAME_FIELD(stat, aquila_stat, st_uid);
SAME_FIELD(stat, aquila_stat, st_gid);
SAME_FIELD(stat, aquila_stat, st_rdev);
SAME_FIELD(stat, aquila_stat, st_size);
SAME_FIELD(stat, aquila_stat, st_atim);
SAME_FIELD(stat, aquila_stat, st_mtim);
SAME_FIELD(stat, aquila_stat, st_ctim);
SAME_FIELD(stat, aquila_stat, st_blksize);
SAME_FIELD(stat, aquila_stat, st_blocks);

SAME_SIZE(sigaction, aquila_sigaction);
_Static_assert(offsetof(struct sigaction, _signal_handlers) == offsetof(struct aquila_sigaction, sa_handler) &&
               sizeof(((struct sigaction *) 0)->_signal_handlers) == sizeof(((struct aquila_sigaction *) 0)->sa_handler),
               "struct sigaction .sa_handler does not match the kernel");
SAME_FIELD(sigaction, aquila_sigaction, sa_mask);
SAME_FIELD(sigaction, aquila_sigaction, sa_flags);

/* the kernel's struct timeval (gettimeofday) is 64-bit seconds, long usec */
_Static_assert(sizeof(struct timeval) == 8 + sizeof(long) &&
               offsetof(struct timeval, tv_usec) == 8,
               "struct timeval does not match the kernel");

/* utimensat(2), nanosleep(2) and clock_gettime(2) take a struct aquila_timespec */
SAME_SIZE(timespec, aquila_timespec);
SAME_FIELD(timespec, aquila_timespec, tv_sec);
SAME_FIELD(timespec, aquila_timespec, tv_nsec);

/* defined by the kernel in <aquila/stat.h> and <aquila/time.h>, which skip
 * them when the C library's headers came first */
_Static_assert(UTIME_NOW == -2 && UTIME_OMIT == -1, "UTIME_NOW/UTIME_OMIT do not match the kernel");
_Static_assert(CLOCK_REALTIME == 1 && CLOCK_MONOTONIC == 4, "clock ids do not match the kernel");

/* times(2) */
SAME_SIZE(tms, aquila_tms);
SAME_FIELD(tms, aquila_tms, tms_utime);
SAME_FIELD(tms, aquila_tms, tms_cstime);

/* waitpid(2) options and the status of a continued child, <aquila/wait.h>
 * skips its definitions after the C library's */
_Static_assert(WNOHANG == 1 && WUNTRACED == 2 && WCONTINUED == 8, "waitpid options do not match the kernel");
_Static_assert(WIFCONTINUED(0xffff) && WIFSTOPPED((SIGTSTP << 8) | 0x7f) &&
               WSTOPSIG((SIGTSTP << 8) | 0x7f) == SIGTSTP, "wait status does not match the kernel");

/* what signal handlers get as their second and third arguments, and
 * sigaltstack()'s stack (the si_code values are checked as the errno
 * numbers are) */
#define SAME_TYPE_FIELD(a, b, f) \
    _Static_assert(offsetof(a, f) == offsetof(struct b, f) && \
                   sizeof(((a *) 0)->f) == sizeof(((struct b *) 0)->f), \
                   #a " ." #f " does not match the kernel")

_Static_assert(sizeof(siginfo_t) == sizeof(struct aquila_siginfo), "siginfo_t size does not match the kernel");
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_signo);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_code);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_errno);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_pid);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_uid);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_status);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_addr);
SAME_TYPE_FIELD(siginfo_t, aquila_siginfo, si_value);

_Static_assert(sizeof(stack_t) == sizeof(struct aquila_stack), "stack_t size does not match the kernel");
SAME_TYPE_FIELD(stack_t, aquila_stack, ss_sp);
SAME_TYPE_FIELD(stack_t, aquila_stack, ss_flags);
SAME_TYPE_FIELD(stack_t, aquila_stack, ss_size);
_Static_assert(SA_ONSTACK == 0x40, "SA_ONSTACK does not match the kernel");

_Static_assert(sizeof(ucontext_t) == sizeof(struct aquila_ucontext), "ucontext_t size does not match the kernel");
SAME_TYPE_FIELD(ucontext_t, aquila_ucontext, uc_link);
SAME_TYPE_FIELD(ucontext_t, aquila_ucontext, uc_sigmask);
SAME_TYPE_FIELD(ucontext_t, aquila_ucontext, uc_stack);
SAME_TYPE_FIELD(ucontext_t, aquila_ucontext, uc_mcontext);
_Static_assert(sizeof(mcontext_t) == sizeof(struct aquila_mcontext), "mcontext_t size does not match the kernel");
#if defined(__x86_64__)
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, rsp);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, r8);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, rip);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, rflags);
#elif defined(__i386__)
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, esp);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, eip);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, eflags);
#elif defined(__aarch64__)
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, x);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, sp);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, pc);
SAME_TYPE_FIELD(mcontext_t, aquila_mcontext, pstate);
#endif
