/*
 * Clocks, nanosleep() and clock_nanosleep(). newlib's sleep() and
 * usleep() use nanosleep() (HAVE_NANOSLEEP).
 */

#include <errno.h>
#include <time.h>

#include <aquila/syscall.h>

#include "syscall.h"

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return SYSCALL(SYS_nanosleep, req, rem, 0);
}

/* POSIX: the error is returned, errno isn't set */
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *req,
                    struct timespec *rem)
{
    long ret = __syscall5(SYS_clock_nanosleep, clock, flags, (long) req, (long) rem, 0);
    return ret < 0 ? -ret : 0;
}

int clock_gettime(clockid_t clock, struct timespec *ts)
{
    return SYSCALL(SYS_clock_gettime, clock, ts, 0);
}

int clock_getres(clockid_t clock, struct timespec *res)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }

    /* the scheduler's timer, 2ms */
    if (res)
        *res = (struct timespec) {0, 2000000};

    return 0;
}

int clock_settime(clockid_t clock, const struct timespec *ts)
{
    /* the kernel's clock can't be set yet */
    errno = clock == CLOCK_REALTIME ? EPERM : EINVAL;
    return -1;
}
