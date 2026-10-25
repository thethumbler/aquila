/** \file
 * The terminal layer: the line discipline, and job control.
 *
 * Every terminal (a virtual terminal, a serial port, a pseudo-terminal)
 * has a `struct tty` between its device's two sides. The master side is
 * the device's (the keyboard, the UART, the program on a pseudo-terminal's
 * master), the slave side the programs that read and write the terminal.
 * Input from the master goes through the line discipline: translation,
 * flow control, signal characters, and in canonical mode line editing,
 * with what is typed echoed. Output from the programs goes through output
 * processing (`OPOST`) to the device. The terminal also belongs to a
 * session, whose foreground process group may read and write it; the
 * other groups are stopped when they try (`SIGTTIN`, `SIGTTOU`). See
 * docs/kernel/devices.md.
 */

#include <core/system.h>
#include <core/arch.h>
#include <sys/sched.h>
#include <dev/tty.h>
#include <sys/uaccess.h>
#include <sys/signal.h>
#include <aquila/fcntl.h>

MALLOC_DEFINE(M_TTY, "tty", "tty structure");
MALLOC_DEFINE(M_TTY_COOK, "tty-cook", "tty cooking buffer");

/* whether `c` is the special character `cc`, which is set (0 disables
 * it: _POSIX_VDISABLE is 0) */
#define IS_CC(tty, c, cc) ((tty)->tios.c_cc[cc] && (c) == (tty)->tios.c_cc[cc])

/** Echoes a character, a control character as ^X. */
static void tty_echo(struct tty *tty, char c)
{
    if ((unsigned char) c < ' ' && c != '\n' && c != '\t') {
        char cc[] = {'^', c + '@'};
        tty_slave_write(tty, 2, cc);
    } else if (c == 0x7f) {
        tty_slave_write(tty, 2, "^?");
    } else {
        tty_slave_write(tty, 1, &c);
    }
}

/** Erases the last character of the line being edited, on the screen
 * too with `ECHOE`. */
static void tty_erase(struct tty *tty)
{
    if (!tty->pos)
        return;

    char c = tty->cook[--tty->pos];

    if ((tty->tios.c_lflag & ECHO) && (tty->tios.c_lflag & ECHOE)) {
        /* a control character was echoed as two characters */
        int width = ((unsigned char) c < ' ' && c != '\t') || c == 0x7f ? 2 : 1;

        while (width--)
            tty_slave_write(tty, 3, "\b \b");
    }
}

/** Hands the line being edited to the readers. */
static void tty_deliver(struct tty *tty)
{
    if (tty->pos)
        tty->slave_write(tty, tty->pos, tty->cook);

    tty->pos = 0;
}

/** Handles a signal character (with `ISIG`): the foreground group gets
 * the signal. Returns whether `c` was one. */
static int tty_signal(struct tty *tty, char c)
{
    int sig = IS_CC(tty, c, VINTR) ? SIGINT :
              IS_CC(tty, c, VQUIT) ? SIGQUIT :
              IS_CC(tty, c, VSUSP) ? SIGTSTP : 0;

    if (!sig)
        return 0;

    if (!(tty->tios.c_lflag & NOFLSH))
        tty->pos = 0;

    if (tty->tios.c_lflag & ECHO) {
        tty_echo(tty, c);
        tty_slave_write(tty, 1, "\n");
    }

    /* the signal is only queued: this runs from the driver, maybe in an
     * interrupt */
    if (tty->fg)
        signal_pgrp_send(tty->fg, sig);

    return 1;
}

/**
 * Takes input from the terminal's device (the keyboard, a UART, or a
 * pseudo-terminal's master), and puts it through the line discipline to
 * what programs read. Canonical mode collects lines, with erase
 * (`VERASE`), kill (`VKILL`) and end-of-file (`VEOF`); otherwise, the
 * programs get the characters as they come.
 *
 * @param tty   the terminal
 * @param size  the input's size, in bytes
 * @param buf   the input
 * @return `size`: all of it is taken (what doesn't fit in a line is dropped)
 */
ssize_t tty_master_write(struct tty *tty, size_t size, void *buf)
{
    struct termios *tios = &tty->tios;
    char *in = buf;

    for (size_t i = 0; i < size; ++i) {
        char c = in[i];

        /* translate CR and NL */
        if (c == '\r') {
            if (tios->c_iflag & IGNCR)
                continue;
            if (tios->c_iflag & ICRNL)
                c = '\n';
        } else if (c == '\n' && (tios->c_iflag & INLCR)) {
            c = '\r';
        }

        /* flow control: VSTOP stops the output, and VSTART (or any
         * character, with IXANY) starts it again; neither is input */
        if (tios->c_iflag & IXON) {
            if (tty->stopped && (IS_CC(tty, c, VSTART) || (tios->c_iflag & IXANY))) {
                tty->stopped = 0;
                waitq_wakeup(&tty->start_wait);

                if (IS_CC(tty, c, VSTART))
                    continue;
            } else if (IS_CC(tty, c, VSTART)) {
                continue;
            } else if (IS_CC(tty, c, VSTOP)) {
                tty->stopped = 1;
                continue;
            }
        }

        if ((tios->c_lflag & ISIG) && tty_signal(tty, c))
            continue;

        if (!(tios->c_lflag & ICANON)) {
            tty->slave_write(tty, 1, &c);

            if (tios->c_lflag & ECHO)
                tty_echo(tty, c);

            continue;
        }

        if (IS_CC(tty, c, VERASE)) {
            tty_erase(tty);
        } else if (IS_CC(tty, c, VKILL)) {
            while (tty->pos)
                tty_erase(tty);
        } else if (IS_CC(tty, c, VEOF)) {
            /* hand over the line so far, without a newline; at the start
             * of a line, the next read returns 0 */
            if (!tty->pos)
                tty->eof++;

            tty_deliver(tty);
        } else if (c == '\n' || IS_CC(tty, c, VEOL)) {
            tty->cook[tty->pos++] = c;

            if ((tios->c_lflag & ECHO) || (c == '\n' && (tios->c_lflag & ECHONL)))
                tty_echo(tty, c);

            tty_deliver(tty);
        } else {
            /* keep room for the line's newline: drop what doesn't fit */
            if (tty->pos + 1 >= tty->size)
                continue;

            tty->cook[tty->pos++] = c;

            if (tios->c_lflag & ECHO)
                tty_echo(tty, c);
        }
    }

    return size;
}

/**
 * Says whether a read that found nothing to return should return 0 at
 * once: at an end-of-file (`VEOF` at the start of a line), or with `VMIN`
 * and `VTIME` both 0 in non-canonical mode. An end-of-file is used up.
 *
 * @param tty  the terminal
 * @return 1 if the read returns 0, 0 if it waits
 */
int tty_eof(struct tty *tty)
{
    /* with VMIN 0 and VTIME set, the read waits (tty_read_timeout()) */
    if (!(tty->tios.c_lflag & ICANON))
        return !tty->tios.c_cc[VMIN] && !tty->tios.c_cc[VTIME];

    if (tty->eof) {
        tty->eof--;
        return 1;
    }

    return 0;
}

/**
 * Says how long a read waits for input: `VTIME` tenths of a second with
 * `VMIN` 0 in non-canonical mode, otherwise until there is some.
 *
 * @param tty  the terminal
 * @return the time, in ns, or 0 for no limit
 */
uint64_t tty_read_timeout(struct tty *tty)
{
    if ((tty->tios.c_lflag & ICANON) || tty->tios.c_cc[VMIN] || !tty->tios.c_cc[VTIME])
        return 0;

    return tty->tios.c_cc[VTIME] * 100000000ULL;
}

/**
 * Says whether the terminal is readable, for `select()` and `poll()`:
 * there is input, or an end-of-file. With `VMIN` and `VTIME` 0 a read
 * doesn't wait either, but it isn't readable without input, as on Linux
 * and the BSDs: a program that waits for input in `select()` would
 * otherwise never sleep.
 *
 * @param tty       the terminal
 * @param buffered  how many bytes the device holds for readers
 * @return whether there is something to read
 */
int tty_can_read(struct tty *tty, size_t buffered)
{
    return buffered || tty->eof;
}

/**
 * Stops a background process that reads the terminal, writes it or
 * changes it: if the current process is of the terminal's session but
 * not of its foreground group, its group gets the signal and stops until
 * it is continued in the foreground. Writes are only stopped with
 * `TOSTOP`, which the callers check.
 *
 * @param tty  the terminal
 * @param sig  `SIGTTIN` for a read, `SIGTTOU` for a write or a change
 * @return 0 if the process may go on; `-ERESTARTSYS` once the signal is
 *         sent (the call returns, the group stops on its way out, and the
 *         call is made again once it is continued); or `-EIO` for a read
 *         with `SIGTTIN` ignored or blocked, or from an orphaned group
 */
int tty_background(struct tty *tty, int sig)
{
    struct proc *proc = curproc;

    if (tty->session && proc->pgrp->session == tty->session &&
        tty->fg && proc->pgrp != tty->fg) {
        if ((proc->sigmask & (1UL << sig)) || proc->sigaction[sig].sa_handler == SIG_IGN)
            return sig == SIGTTIN ? -EIO : 0;

        /* an orphaned group isn't stopped: no one would continue it */
        if (pgrp_orphaned(proc->pgrp, NULL))
            return -EIO;

        signal_pgrp_send(proc->pgrp, sig);
        return -ERESTARTSYS;
    }

    return 0;
}

/** Sends `n` bytes to the device. Returns whether it took all of them. */
static int tty_out(struct tty *tty, const char *s, size_t n)
{
    return tty->master_write(tty, n, (void *) s) == (ssize_t) n;
}

/**
 * Takes what programs write to the terminal, and sends it to the device
 * through output processing (`OPOST`, POSIX's output modes): NL as CR NL
 * (`ONLCR`), CR as NL (`OCRNL`), no CR in the first column (`ONOCR`), and
 * NL returning the carriage too (`ONLRET`). The column is tracked for
 * them.
 *
 * @param tty    the terminal
 * @param size   the output's size, in bytes
 * @param _buf   the output
 * @return how many bytes of it went (the device may take fewer)
 */
ssize_t tty_slave_write(struct tty *tty, size_t size, void *_buf)
{
    const char *buf = (const char *) _buf;
    tcflag_t oflag = tty->tios.c_oflag;

    if (!(oflag & OPOST))
        return tty->master_write(tty, size, _buf);

    size_t written;

    for (written = 0; written < size; ++written) {
        char c = buf[written];

        if (c == '\n') {
            if (oflag & ONLCR) {
                /* CR NL; if only the CR went last time, just the NL now */
                if (tty->nl_owed) {
                    if (!tty_out(tty, "\n", 1))
                        break;
                } else {
                    ssize_t r = tty->master_write(tty, 2, "\r\n");

                    if (r != 2) {
                        tty->nl_owed = r == 1;
                        if (r == 1)
                            tty->col = 0;
                        break;
                    }
                }

                tty->nl_owed = 0;
                tty->col = 0;
            } else {
                if (!tty_out(tty, "\n", 1))
                    break;

                if (oflag & ONLRET)
                    tty->col = 0;
            }
        } else if (c == '\r') {
            /* in the first column, there is nothing to return from */
            if ((oflag & ONOCR) && !tty->col)
                continue;

            if (oflag & OCRNL) {
                if (!tty_out(tty, "\n", 1))
                    break;

                if (oflag & ONLRET)
                    tty->col = 0;
            } else {
                if (!tty_out(tty, "\r", 1))
                    break;

                tty->col = 0;
            }
        } else {
            if (!tty_out(tty, &c, 1))
                break;

            if (c == '\t')
                tty->col = (tty->col | 7) + 1;
            else if (c == '\b')
                tty->col -= !!tty->col;
            else if ((unsigned char) c >= ' ' && c != 0x7f)
                tty->col++;
        }
    }

    return written;
}

static void tty_release(struct tty *tty);

/**
 * Waits while the terminal's output is stopped (`IXON`). Returns 0 once it
 * starts again, `-ERESTARTSYS` for a signal, or `-EIO` if the terminal
 * went away meanwhile (and its last waiter then frees it).
 */
static int tty_wait_start(struct tty *tty)
{
    int err = 0;

    while (tty->stopped && !tty->gone && !err) {
        tty->start_waiters++;
        err = waitq_sleep(&tty->start_wait);
        tty->start_waiters--;
    }

    if (tty->gone) {
        if (!tty->start_waiters)
            tty_release(tty);

        return err ? err : -EIO;
    }

    return err;
}

/**
 * Writes to a terminal from an iobuf (user memory), to either side: a
 * piece at a time through a kernel buffer, the iobuf moved on only as far
 * as was taken.
 *
 * @param tty    the terminal
 * @param io     what to write
 * @param write  the side: `tty_master_write` (input, from a
 *               pseudo-terminal's master) or `tty_slave_write` (a
 *               program's output)
 * @return how many bytes were written, or a negative errno
 */
ssize_t tty_write_iobuf(struct tty *tty, struct iobuf *io, ttyio write)
{
    char buf[256];
    size_t size = io->resid, done = 0;

    /* a program's output to a stopped terminal waits */
    if (write == tty_slave_write && tty->stopped) {
        int err = tty_wait_start(tty);

        if (err)
            return err;
    }

    while (done < size) {
        ssize_t len = iobuf_peek(io, buf, MIN(size - done, sizeof(buf)));

        if (len <= 0)
            return done ? (ssize_t) done : len;

        ssize_t r = write(tty, len, buf);

        if (r < 0)
            return done ? (ssize_t) done : r;

        iobuf_skip(io, r);
        done += r;

        /* the terminal took no more, or a fault stopped the copy */
        if (r < len || io->fault)
            break;
    }

    return done;
}

/**
 * Handles a terminal's `ioctl()`s: its modes (`TCGETS`, `TCSETS`, ...),
 * flushing, its session and foreground group, its window size, and
 * becoming the controlling terminal (`TIOCSCTTY`).
 *
 * @param tty      the terminal
 * @param request  the request
 * @param argp     its argument, in user memory (or the value itself, for
 *                 `TCFLSH`)
 * @return 0 or what the request returns, or a negative errno (`-ENOTTY`
 *         for a request terminals don't have)
 */
int tty_ioctl(struct tty *tty, int request, void *argp)
{
    int err;

    switch (request) {
        case TCGETS:
            return copyout(argp, &tty->tios, sizeof(struct termios));
        case TCSETS:
        case TCSETSW:   /* output is never waiting to be sent */
        case TCSETSF: {
            struct termios tios;

            if (copyin(&tios, argp, sizeof(tios)))
                return -EFAULT;

            if ((err = tty_background(tty, SIGTTOU)))
                return err;

            if (request == TCSETSF)
                tty_flush(tty, TCIFLUSH);

            tty->tios = tios;

            /* no flow control anymore: start what was stopped */
            if (!(tios.c_iflag & IXON) && tty->stopped) {
                tty->stopped = 0;
                waitq_wakeup(&tty->start_wait);
            }
            break;
        }
        case TCFLSH:
            /* the argument is the value itself: TCIFLUSH, TCOFLUSH or
             * TCIOFLUSH */
            if ((uintptr_t) argp > TCIOFLUSH)
                return -EINVAL;

            tty_flush(tty, (uintptr_t) argp);
            break;
        case TCSBRK:
        case TCXONC:
            /* no output waits to be sent, and there's no flow control to
             * apply */
            break;
        case TIOCGSID:
            if (!tty->session)
                return -ENOTTY;

            return copyout(argp, &tty->session->sid, sizeof(pid_t));
        case TIOCGPGRP: {
            /* without a foreground group, a value that is no process
             * group, as POSIX says */
            pid_t pgid = tty->fg ? tty->fg->pgid : 0x7fffffff;
            return copyout(argp, &pgid, sizeof(pgid));
        }
        case TIOCSPGRP: {
            /* only on the caller's controlling terminal, and to a group of
             * its session */
            pid_t pgid;
            struct session *session = curproc->pgrp->session;
            struct pgroup *pgrp = NULL;

            if (copyin(&pgid, argp, sizeof(pgid)))
                return -EFAULT;

            if (session->ctty != tty)
                return -ENOTTY;

            if ((err = tty_background(tty, SIGTTOU)))
                return err;

            queue_for (node, session->pgps) {
                struct pgroup *pg = node->value;
                if (pg->pgid == pgid) {
                    pgrp = pg;
                    break;
                }
            }

            if (!pgrp)
                return -EPERM;

            tty->fg = pgrp;
            break;
        }
        case TIOCGWINSZ:
            return copyout(argp, &tty->ws, sizeof(struct winsize));
        case TIOCSWINSZ: {
            struct winsize ws;

            if (copyin(&ws, argp, sizeof(ws)))
                return -EFAULT;

            tty->ws = ws;
            break;
        }
        case TIOCSCTTY: {
            /* a session leader without a controlling terminal takes one
             * that no other session has */
            struct session *session = curproc->pgrp->session;

            if (session->leader != curproc || session->ctty || tty->session)
                return -EPERM;

            session->ctty = tty;
            tty->session  = session;
            tty->fg       = curproc->pgrp;
            break;
        }
        default:
            /* a request that terminals don't have, as POSIX and Linux
             * say */
            return -ENOTTY;
    }
    
    return 0;
}

/**
 * Makes a new terminal, with the default modes (canonical, echo, signals,
 * `ICRNL`, `IXON`, `OPOST` with `ONLCR`) and the usual special characters.
 *
 * @param proc      the process making it (not used)
 * @param buf_size  the size of its line buffer, or 0 for `TTY_BUF_SIZE`
 * @param master    how the terminal sends output to its device
 * @param slave     how the terminal hands input to its readers
 * @param p         the device's own data, for those two
 * @param ref       set to the new terminal
 * @return 0, or `-ENOMEM`
 */
int tty_new(struct proc *proc, size_t buf_size, ttyio master,
        ttyio slave, void *p, struct tty **ref)
{
    struct tty *tty = NULL;
    
    tty = kmalloc(sizeof(struct tty), &M_TTY, M_ZERO);
    if (!tty) return -ENOMEM;

    if (!buf_size)
        buf_size = TTY_BUF_SIZE;

    tty->cook = kmalloc(buf_size, &M_TTY_COOK, 0);

    if (!tty->cook) {
        kfree(tty);
        return -ENOMEM;
    }

    tty->size = buf_size;
    tty->pos  = 0;
    tty->col  = 0;
    tty->nl_owed = 0;

    /* the defaults */
    tty->tios.c_iflag = ICRNL | IXON;
    tty->tios.c_oflag = OPOST | ONLCR;
    tty->tios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    tty->tios.c_cc[VERASE] = 0x7f;  /* DEL, what backspace sends */
    tty->tios.c_cc[VEOF]   = 0x04;  /* ^D */
    tty->tios.c_cc[VMIN]   = 1;
    tty->tios.c_cc[VINTR]  = 0x03;  /* ^C */
    tty->tios.c_cc[VKILL]  = 0x15;  /* ^U */
    tty->tios.c_cc[VQUIT]  = 0x1C;  /* ^\ */
    tty->tios.c_cc[VSTART] = 0x11;  /* ^Q */
    tty->tios.c_cc[VSTOP]  = 0x13;  /* ^S */
    tty->tios.c_cc[VSUSP]  = 0x1A;  /* ^Z */

    tty->ws.ws_row = 24;
    tty->ws.ws_col = 80;

    /* the device's side */
    tty->master_write = master;
    tty->slave_write  = slave;
    tty->p = p;

    if (ref)
        *ref = tty;

    return 0;
}

/**
 * Handles a terminal device being opened: it becomes the controlling
 * terminal of a session leader that has none (unless `O_NOCTTY`), if no
 * session has it already. Opening `/dev/console` doesn't do it, as on
 * Linux: init writes there, and would otherwise take the terminal that is
 * also the console (the first virtual terminal, or the UART) from the
 * session that a getty starts on it.
 *
 * @param tty   the terminal
 * @param file  the file being opened
 */
void tty_open(struct tty *tty, struct file *file)
{
    struct session *session = curproc->pgrp->session;
    int console = file->vnode && file->vnode->rdev == DEV(5, 1);

    if ((file->flags & O_NOCTTY) || console || session->leader != curproc ||
        session->ctty || tty->session)
        return;

    session->ctty = tty;
    tty->session  = session;
    tty->fg       = curproc->pgrp;
}

/**
 * Hangs up a controlling terminal, when its session ends (its leader
 * exited) or the terminal goes away: its foreground group gets `SIGHUP`
 * (and `SIGCONT`, in case it was stopped), and the terminal belongs to no
 * session anymore.
 *
 * @param tty  the terminal
 */
void tty_hangup(struct tty *tty)
{
    if (!tty->session)
        return;

    if (tty->fg) {
        signal_pgrp_send(tty->fg, SIGHUP);
        signal_pgrp_send(tty->fg, SIGCONT);
    }

    tty->session->ctty = NULL;
    tty->session = NULL;
    tty->fg = NULL;
}

/**
 * Drops a terminal's input (`TCIFLUSH`, `TCIOFLUSH`): the line being
 * edited, what the device holds for readers, and the pending
 * end-of-files. Output isn't kept anywhere, so there is none to drop.
 *
 * @param tty    the terminal
 * @param which  `TCIFLUSH`, `TCOFLUSH` or `TCIOFLUSH`
 */
void tty_flush(struct tty *tty, int which)
{
    if (which == TCOFLUSH)
        return;

    tty->pos = 0;
    tty->eof = 0;

    if (tty->input_flush)
        tty->input_flush(tty);
}

/** Frees a terminal's memory. */
static void tty_release(struct tty *tty)
{
    kfree(tty->cook);
    kfree(tty);
}

/**
 * Ends a terminal: it is hung up, and freed. If writers are waiting for
 * its output to start again, they are woken up instead, and the last one
 * frees it.
 *
 * @param tty  the terminal
 * @return 0
 */
int tty_free(struct tty *tty)
{
    tty_hangup(tty);

    /* writers waiting for VSTART: wake them, and the last one frees it */
    if (tty->start_waiters) {
        tty->gone = 1;
        tty->stopped = 0;
        waitq_wakeup(&tty->start_wait);
        return 0;
    }

    tty_release(tty);
    return 0;
}
