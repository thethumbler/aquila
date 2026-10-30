/** \file
 * System calls that wait for any of several descriptors: `select()` (and
 * `pselect()`) and `poll()`.
 *
 * Both look at each descriptor, registering the wait queue its readers
 * (or writers) sleep on. If none is ready, the thread sleeps on its own
 * queue (`thread->poll_wait`), which a wakeup of any registered queue
 * also wakes, and then looks at them all again. See `sys/waitq.c`.
 */

#include "syscalls.h"

/** Returns the open file a descriptor refers to, without holding it (as
 * `fd_file()` does): it is only used until the call sleeps. */
static struct file *fd_peek(int fd)
{
    return proc_fd_file(curproc, fd);
}

typedef unsigned long fd_mask;

#define  FD_SETSIZE  64
#define   NFDBITS (sizeof (fd_mask) * 8)  /* the bits in an fd_mask */
#define  _howmany(x,y)   (((x)+((y)-1))/(y))

/* the C library's fd_set: one bit per descriptor */
typedef struct {
    fd_mask fds_bits[_howmany(FD_SETSIZE, NFDBITS)];
} fd_set;

/** Returns whether `file` can be read (or written) without waiting. A file
 * that can't tell counts as ready: the read or write reports the error. */
static int fd_ready(struct file *file, int write)
{
    int ret = write ? vfs_file_can_write(file, 1) : vfs_file_can_read(file, 1);
    return ret != 0;
}

/** Registers the wait queue that `file`'s readers (or writers) sleep on,
 * or NULL (any wakeup) if the file can't tell. Returns 0 or `-ENOMEM`. */
static int fd_watch(struct file *file, int write)
{
    return poll_register(vfs_file_wait_queue(file, write));
}

/** Returns the deadline of a timeout of `ns` ns: 0 for none (`ns` < 0). A
 * timeout of 0 means not to wait at all. A call restarted after a stop
 * keeps the deadline it had. */
static uint64_t poll_deadline(int64_t ns)
{
    uint64_t until = ns < 0 ? 0 : curthread->restarting ? curthread->restart_until :
                     arch_rtime_ns() + ns;

    curthread->restart_until = until;
    return until;
}

/**
 * Implements `select()` and `pselect()`: waits until a descriptor of the
 * sets can be read or written, or the timeout passes, and says which can.
 * Exceptional conditions are never reported.
 *
 * @param args  the arguments, in user memory: `nfds`, the read, write and
 *              except sets, the timeout, and `pselect()`'s signal mask
 * @return how many descriptors are ready (0 on timeout), or a negative
 *         errno: `-EBADF`, `-EINVAL`, `-EFAULT`, or `-EINTR`
 */
long sys_select(struct aquila_select_args *args)
{
    syscall_log(LOG_DEBUG, "select(args=%p)\n", args);

    struct aquila_select_args a;

    if (copyin(&a, args, sizeof(a))) {
        return -EFAULT;
    }

    int nfds = a.nfds;
    fd_set *readfds = a.readfds;
    fd_set *writefds = a.writefds;
    fd_set *exceptfds = a.exceptfds;
    sigset_t *sigmask = a.sigmask;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        return -EINVAL;
    }

    /* the kernel's copies of what was asked (the user's sets are
     * overwritten with the result) */
    fd_set want_read = {0}, want_write = {0}, out_read, out_write;
    struct timeval timeout;
    sigset_t mask;

    if ((readfds && copyin(&want_read, readfds, sizeof(fd_set))) ||
        (writefds && copyin(&want_write, writefds, sizeof(fd_set))) ||
        (a.timeout && copyin(&timeout, a.timeout, sizeof(timeout))) ||
        (sigmask && copyin(&mask, sigmask, sizeof(mask)))) {
        return -EFAULT;
    }

    if (a.timeout && ((int64_t) timeout.tv_sec < 0 || timeout.tv_usec < 0 || timeout.tv_usec >= 1000000)) {
        return -EINVAL;
    }

    /* pselect(): the signal mask while waiting; the old mask comes back
     * when this returns, or goes in the handler's frame (as in
     * sigsuspend()) */
    if (sigmask) {
        curthread->sig_oldmask = curthread->sigmask;
        curthread->sig_oldmask_set = 1;
        curthread->sigmask = mask & ~SIG_UNMASKABLE;
    }

    int64_t ns = a.timeout ? (int64_t) timeout.tv_sec * 1000000000 + timeout.tv_usec * 1000 : -1;
    uint64_t until = poll_deadline(ns);
    int count, timed_out = 0;

#define FD_BIT(set, i) ((set).fds_bits[(i) / NFDBITS] & (1UL << ((i) % NFDBITS)))
#define FD_ADD(set, i) ((set).fds_bits[(i) / NFDBITS] |= (1UL << ((i) % NFDBITS)))

    for (;;) {
        out_read  = (fd_set) {0};
        out_write = (fd_set) {0};
        count = 0;

        for (int i = 0; i < nfds; ++i) {
            if (!FD_BIT(want_read, i) && !FD_BIT(want_write, i))
                continue;

            struct file *file = fd_peek(i);
            int err = 0;

            if (!file)
                err = -EBADF;

            if (!err && FD_BIT(want_read, i) && !(err = fd_watch(file, 0)) && fd_ready(file, 0)) {
                FD_ADD(out_read, i);
                ++count;
            }

            if (!err && FD_BIT(want_write, i) && !(err = fd_watch(file, 1)) && fd_ready(file, 1)) {
                FD_ADD(out_write, i);
                ++count;
            }

            if (err) {
                poll_unregister(curthread);
                return err;
            }
        }

        if (count || ns == 0 || timed_out) {
            poll_unregister(curthread);
            break;
        }

        /* a wakeup of a queue that one of them waits on makes us look
         * again; a signal ends the wait with EINTR (POSIX: it isn't
         * restarted, except after a stop) */
        timed_out = sched_wait(&curthread->poll_wait, until);
        poll_unregister(curthread);

        if (timed_out < 0) {
            return -ERESTARTNOHAND;
        }
    }

    if (sigmask) {
        curthread->sigmask = curthread->sig_oldmask;
        curthread->sig_oldmask_set = 0;
    }

#undef FD_BIT
#undef FD_ADD

    fd_set none = {0};

    if ((readfds && copyout(readfds, &out_read, sizeof(fd_set))) ||
        (writefds && copyout(writefds, &out_write, sizeof(fd_set))) ||
        (exceptfds && copyout(exceptfds, &none, sizeof(fd_set)))) {
        return -EFAULT;
    }

    return count;
}

/**
 * Implements `poll()`: waits until one of the descriptors can be read or
 * written, or the timeout passes, and says which can.
 *
 * @param fds         the descriptors and the events wanted, in user
 *                    memory; their `revents` are set
 * @param nfds        how many there are
 * @param timeout_ms  the timeout in ms, or -1 for none
 * @return how many descriptors have events (0 on timeout), or a negative
 *         errno: `-EINVAL`, `-ENOMEM`, `-EFAULT`, or `-EINTR`
 */
long sys_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    syscall_log(LOG_DEBUG, "poll(fds=%p, nfds=%d, timeout=%d)\n", fds, nfds, timeout_ms);

    if (nfds > FDS_COUNT * 2) {
        return -EINVAL;
    }

    /* work on the kernel's copy, and copy the results out at the end */
    size_t size = nfds * sizeof(struct pollfd);
    struct pollfd *kfds = nfds ? kmalloc(size, &M_BUFFER, 0) : NULL;

    if (nfds && !kfds) {
        return -ENOMEM;
    }

    if (copyin(kfds, fds, size)) {
        kfree(kfds);
        return -EFAULT;
    }

    int64_t ns = timeout_ms < 0 ? -1 : (int64_t) timeout_ms * 1000000;
    uint64_t until = poll_deadline(ns);
    int count, timed_out = 0;

    for (;;) {
        int err = 0;
        count = 0;

        for (nfds_t i = 0; i < nfds; ++i) {
            struct pollfd *p = &kfds[i];
            struct file *file;

            p->revents = 0;

            /* a negative descriptor is skipped */
            if (p->fd < 0)
                continue;

            if (!(file = fd_peek(p->fd))) {
                p->revents = POLLNVAL;
            } else {
                if ((p->events & POLLIN) && !err && !(err = fd_watch(file, 0)) && fd_ready(file, 0))
                    p->revents |= POLLIN;

                if ((p->events & POLLOUT) && !err && !(err = fd_watch(file, 1)) && fd_ready(file, 1))
                    p->revents |= POLLOUT;
            }

            count += !!p->revents;
        }

        if (err) {
            poll_unregister(curthread);
            count = err;
            break;
        }

        if (count || ns == 0 || timed_out) {
            poll_unregister(curthread);
            break;
        }

        /* a wakeup of a queue that one of them waits on makes us look
         * again; a signal ends the wait with EINTR (except after a stop) */
        timed_out = sched_wait(&curthread->poll_wait, until);
        poll_unregister(curthread);

        if (timed_out < 0) {
            count = -ERESTARTNOHAND;
            break;
        }
    }

    if (count >= 0 && copyout(fds, kfds, size))
        count = -EFAULT;

    kfree(kfds);
    return count;
}
