/** \file
 * Pseudo-terminals: `/dev/ptmx` and `/dev/pts/N`.
 *
 * A pseudo-terminal is a terminal with a program on both sides. Opening
 * `/dev/ptmx` (character device 5, 2) makes a new one and gives its
 * master side: what the program there (a terminal emulator, `script`, an
 * SSH server) writes is the terminal's input, and it reads the terminal's
 * output. The slave side, `/dev/pts/N` in devpts, is the terminal that a
 * shell runs on. The terminal layer (`generic.c`) does the line editing
 * in between. There is a ring each way. Closing the master's last
 * descriptor ends the pseudo-terminal.
 */

#include <core/system.h>
#include <sys/uaccess.h>
#include <core/module.h>
#include <fs/vfs.h>
#include <fs/devpts.h>
#include <fs/posix.h>
#include <dev/dev.h>
#include <dev/tty.h>
#include <sys/sched.h>
#include <aquila/errno.h>
#include <ds/ringbuf.h>

MALLOC_DEFINE(M_PTY, "pseudo-terminal", "pseudo-terminal structure");

/** A pseudo-terminal. */
struct pty {
    size_t id;
    struct tty *tty;

    struct vnode *master;
    struct vnode *slave;

    /** what the master writes, after line editing, for the slave to read */
    struct ringbuf *in;

    /** what the slave writes, after output processing, for the master to
     *  read */
    struct ringbuf *out;

    /** where the slave's readers sleep, and the writers waiting for room
     *  in either ring (pty_slave_write(), pty_master_write()) */
    struct waitq pts_read_queue;

    /** where the slave's writers and the master's readers sleep */
    struct waitq pts_write_queue;
};

#define PTY_BUF 4096
#define PTY_MAX 256

static struct pty *ptys[PTY_MAX] = {0};

/* which pseudo-terminal numbers are taken (a pseudo-terminal may not be
 * fully set up yet) */
static char pty_ids[PTY_MAX] = {0};

struct dev ptmdev;
struct dev ptsdev;

/** Returns the pseudo-terminal of a master (major 2) or slave (major 136)
 * device, or NULL if there is none. */
static struct pty *pty_get(struct devid *dd)
{
    if (dd->major != 2 && dd->major != 136)
        return NULL;

    /* the minor is a devid_t (8 bits), so it is always below PTY_MAX */
    return ptys[dd->minor];
}

/**
 * Takes the terminal's output, for the master to read. It waits while
 * the ring is full, until all of it is taken or a signal comes.
 *
 * @param tty   the terminal
 * @param size  the output's size, in bytes
 * @param buf   the output
 * @return how much was taken, or `-ERESTARTSYS` if a signal came before
 *         any was
 */
ssize_t pty_master_write(struct tty *tty, size_t size, void *buf)
{
    struct pty *pty = (struct pty *) tty->p;
    size_t written = 0;

    while (written < size) {
        written += ringbuf_write(pty->out, size - written, (char *) buf + written);

        if (written != size) {
            /* a write always completes, unless a signal comes */
            int err = waitq_sleep(&pty->pts_read_queue);

            if (err)
                return written ? (ssize_t) written : err;
        }
    }

    return written;
}

/**
 * Takes the terminal's input, after its line editing, for the slave to
 * read. It waits while the ring is full, until all of it is taken or a
 * signal comes.
 *
 * @param tty   the terminal
 * @param size  the input's size, in bytes
 * @param buf   the input
 * @return how much was taken, or `-ERESTARTSYS` if a signal came before
 *         any was
 */
ssize_t pty_slave_write(struct tty *tty, size_t size, void *buf)
{
    struct pty *pty = (struct pty *) tty->p;
    size_t written = 0;

    while (written < size) {
        written += ringbuf_write(pty->in, size - written, (char *) buf + written);

        if (written != size) {
            /* a write always completes, unless a signal comes */
            int err = waitq_sleep(&pty->pts_read_queue);

            if (err)
                return written ? (ssize_t) written : err;
        }
    }

    return written;
}

/** Drops the input that the slave hasn't read (`tcflush()`). */
static void pty_input_flush(struct tty *tty)
{
    struct pty *pty = (struct pty *) tty->p;
    char c;

    while (ringbuf_read(pty->in, 1, &c))
        ;
}

/*
 * The master side
 */

/** Makes the master's vnode: an anonymous file, in no filesystem.
 * Returns 0, or `-ENOMEM`. */
static int ptm_new(struct pty *pty, struct vnode **ref)
{
    struct vnode *ptm = NULL;
    
    ptm = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
    if (ptm == NULL) return -ENOMEM;

    ptm->rdev        = DEV(2, pty->id);
    ptm->mode        = S_IFCHR;
    ptm->ref         = 1;   /* the descriptor /dev/ptmx was opened with */
    ptm->read_queue  = &pty->pts_read_queue;
    ptm->write_queue = &pty->pts_write_queue;
    ptm->p           = pty;

    if (ref)
        *ref = ptm;

    return 0;
}

/** Reads the terminal's output, and wakes up the writers waiting for room. */
static ssize_t ptm_read(struct devid *dd, off_t offset __unused, struct iobuf *io)
{
    struct pty *pty = pty_get(dd);
    if (!pty) return -ENXIO;

    ssize_t ret = ringbuf_read_iobuf(pty->out, io->resid, io);

    /* only if there's room now: the master's readers sleep on this queue
     * too, and two waiting for output would wake each other up for good */
    if (ret > 0)
        waitq_wakeup(&pty->pts_read_queue);

    return ret;
}

/** Writes to the terminal, as typed input. */
static ssize_t ptm_write(struct devid *dd, off_t offset __unused, struct iobuf *io)
{
    struct pty *pty = pty_get(dd);
    if (!pty) return -ENXIO;

    return tty_write_iobuf(pty->tty, io, tty_master_write);
}

/** Handles a pseudo-terminal's `ioctl()`s: its number (`TIOCGPTN`), its
 * lock (`TIOCSPTLCK`), and the terminal's own. */
static int pty_ioctl(struct devid *dd, int request, void *argp)
{
    struct pty *pty = pty_get(dd);
    if (!pty) return -ENXIO;

    switch (request) {
        case TIOCGPTN:
            return copyout(argp, &pty->id, sizeof(int));
        case TIOCSPTLCK: {
            /* the lock isn't kept: the slave is always unlocked */
            int lock;
            return copyin(&lock, argp, sizeof(lock));
        }
    }

    return tty_ioctl(pty->tty, request, argp);  
}

/*
 * The slave side
 */

/** Makes the slave's node, `/dev/pts/N`, owned by the process that opened
 * the master. Returns 0 or a negative errno. */
static int pts_new(struct pty *pty, struct vnode **ref)
{
    char name[12];
    memset(name, 0, 12);
    snprintf(name, 11, "%d", pty->id);

    dev_t dev = DEV(136 + pty->id / 256, pty->id % 256);

    /* the opener's, and writable by the group like other terminals */
    struct uio uio = {.uid = curproc->uid, .gid = curproc->gid};

    struct vnode *pts = NULL;
    int err = 0;
    if ((err = vfs_vmknod(devpts_root, name, S_IFCHR | 0620, dev, &uio, &pts)))
        return err;

    pts->read_queue = &pty->pts_read_queue;
    pts->write_queue = &pty->pts_write_queue;

    if (ref)
        *ref = pts;

    return 0;
}

/**
 * Reads the slave's input. A process of a background group gets `SIGTTIN`
 * instead.
 *
 * @param dd      the device's number
 * @param offset  not used: a terminal has no offset
 * @param io      where to put what is read
 * @return how much was read, or a negative errno
 */
ssize_t pts_read(struct devid *dd, off_t offset __unused, struct iobuf *io)
{
    struct pty *pty = pty_get(dd);
    if (!pty) return -ENXIO;

    int err = tty_background(pty->tty, SIGTTIN);
    if (err) return err;

    ssize_t ret = ringbuf_read_iobuf(pty->in, io->resid, io);

    /* there is room in the input now: wake up the master's writers that
     * wait for some (pty_slave_write() sleeps on this queue) */
    if (ret > 0)
        waitq_wakeup(&pty->pts_read_queue);

    return ret;
}

/**
 * Writes to the slave: the terminal's output. With `TOSTOP` set, a process
 * of a background group gets `SIGTTOU` instead.
 *
 * @param dd      the device's number
 * @param offset  not used: a terminal has no offset
 * @param io      what to write
 * @return how much was written, or a negative errno
 */
ssize_t pts_write(struct devid *dd, off_t offset __unused, struct iobuf *io)
{
    struct pty *pty = pty_get(dd);
    if (!pty) return -ENXIO;

    if (pty->tty->tios.c_lflag & TOSTOP) {
        int err = tty_background(pty->tty, SIGTTOU);

        if (err)
            return err;
    }

    return tty_write_iobuf(pty->tty, io, tty_slave_write);
}

/** Returns whether a read of the slave that found no data is at the end
 * of the file (the terminal says, from its mode). */
static int pts_eof(struct file *file)
{
    struct pty *pty = pty_get(&VNODE_DEV(file->vnode));
    return !pty || tty_eof(pty->tty);
}

/**
 * Says whether a read of the slave wouldn't have to wait.
 *
 * @param file  the open slave
 * @param size  how much would be read
 * @return whether it can be read
 */
int pts_can_read(struct file *file, size_t size)
{
    struct devid *dd = &VNODE_DEV(file->vnode);
    struct pty *pty = pty_get(dd);
    if (!pty) return 1; /* let the read report the error */

    return tty_can_read(pty->tty, ringbuf_available(pty->in));
}

/** Returns whether the master can be read: the terminal output something. */
static int ptm_can_read(struct file *file, size_t size)
{
    struct pty *pty = pty_get(&VNODE_DEV(file->vnode));
    return !pty || ringbuf_available(pty->out);
}

/** Returns how long a read of the slave may wait, from the terminal's
 * mode (`VTIME`), or 0 for no limit. */
static uint64_t pts_read_timeout(struct file *file)
{
    struct pty *pty = pty_get(&VNODE_DEV(file->vnode));
    return pty ? tty_read_timeout(pty->tty) : 0;
}

/*
 * Pseudo-terminals
 */

/** Takes the first free pseudo-terminal number. Returns it, or `-ENOSPC`
 * if every one is taken. */
static inline int pty_id_get(void)
{
    for (int id = 0; id < PTY_MAX; ++id) {
        if (!pty_ids[id]) {
            pty_ids[id] = 1;
            return id;
        }
    }

    return -ENOSPC;
}

/**
 * Makes a new pseudo-terminal: its rings, its terminal, its master and its
 * slave, `/dev/pts/N`.
 *
 * @param proc    the process making it
 * @param master  set to the master's vnode
 * @return 0, or a negative errno (`-ENOSPC` if every number is taken)
 */
int pty_new(struct proc *proc, struct vnode **master)
{
    int err = 0, id = -1;
    struct pty *pty = NULL;

    if ((id = pty_id_get()) < 0)
        return id;

    pty = kmalloc(sizeof(struct pty), &M_PTY, M_ZERO);
    if (!pty) goto e_nomem;

    pty->id = id;

    if (!(pty->in = ringbuf_new(PTY_BUF))) {
        err = -ENOMEM;
        goto error;
    }

    if (!(pty->out = ringbuf_new(PTY_BUF))) {
        err = -ENOMEM;
        goto error;
    }

    if ((err = tty_new(proc, 0, pty_master_write, pty_slave_write, pty, &pty->tty)))
        goto error;

    pty->tty->dev = &ptmdev;

    if ((err = ptm_new(pty, &pty->master)))
        goto error;

    if ((err = pts_new(pty, &pty->slave)))
        goto error;

    pty->tty->vnode = pty->slave;
    pty->tty->input_flush = pty_input_flush;
    ptys[pty->id] = pty;

    if (master)
        *master = pty->master;

    return 0;

e_nomem:
    err = -ENOMEM;
error:
    if (pty) {
        if (pty->in)
            ringbuf_free(pty->in);
        if (pty->out)
            ringbuf_free(pty->out);
        if (pty->tty)
            tty_free(pty->tty);
        if (pty->master)
            kfree(pty->master);

        kfree(pty);
    }

    pty_ids[id] = 0;

    return err;
}

/** Ends a pseudo-terminal, once its master is closed: the terminal, its
 * `/dev/pts` node and its number go. */
static void pty_free(struct pty *pty)
{
    char name[12] = {0};
    struct uio uio = {0};

    ptys[pty->id] = NULL;

    /* hang up the session it controls */
    tty_free(pty->tty);

    /* the slaves still open fail with ENXIO from now on: wake up those
     * waiting on them */
    waitq_wakeup(&pty->pts_read_queue);
    waitq_wakeup(&pty->pts_write_queue);
    pty->slave->read_queue  = NULL;
    pty->slave->write_queue = NULL;

    snprintf(name, 11, "%d", pty->id);
    vfs_vunlink(devpts_root, name, &uio);
    vfs_close(pty->slave);

    ringbuf_free(pty->in);
    ringbuf_free(pty->out);
    pty_ids[pty->id] = 0;
    kfree(pty);
}

/**
 * Opens `/dev/ptmx`: a new pseudo-terminal is made, and the file is its
 * master instead.
 *
 * @param file  the file being opened
 * @return 0, or a negative errno
 */
int ptmx_open(struct file *file)
{
    struct vnode *ptmx = file->vnode;
    int err = pty_new(curproc, &file->vnode);

    /* the file is the new master now */
    if (!err)
        vfs_close(ptmx);

    return err;
}

/** Closes the master. It is in no filesystem, so its references are
 * counted here; the last close ends the pseudo-terminal. */
static int ptm_close(struct file *file)
{
    struct vnode *ptm = file->vnode;

    if (--ptm->ref)
        return 0;

    pty_free(ptm->p);
    kfree(ptm);
    return 0;
}

/** Opens the slave, which may become the process's controlling terminal. */
static int pts_open(struct file *file)
{
    struct pty *pty = pty_get(&VNODE_DEV(file->vnode));

    if (!pty)
        return -ENXIO;

    tty_open(pty->tty, file);
    return posix_file_open(file);
}

/**
 * Registers the pseudo-terminals' devices: the masters (major 2) and the
 * slaves (majors 136 to 143).
 *
 * @return 0
 */
int pty_init()
{
    kdev_chrdev_register(2, &ptmdev);

    for (devid_t i = 136; i < 144; ++i)
        kdev_chrdev_register(i, &ptsdev);

    return 0;
}

struct dev ptsdev = {
    .name  = "ptsdev",
    .read  = pts_read,
    .write = pts_write,
    .ioctl = pty_ioctl,

    .fops = {
        .open  = pts_open,
        .read  = posix_file_read,
        .write = posix_file_write,
        .ioctl = posix_file_ioctl,

        .can_read  = pts_can_read,
        .can_write = __vfs_can_always,
        .eof       = pts_eof,
        .read_timeout = pts_read_timeout,
    }
};

struct dev ptmdev = {
    .name = "ptmdev",
    .read  = ptm_read,
    .write = ptm_write,
    .ioctl = pty_ioctl,

    .fops = {
        .open  = ptmx_open,
        .close = ptm_close,
        .read  = posix_file_read,
        .write = posix_file_write,
        .ioctl = posix_file_ioctl,

        .can_read  = ptm_can_read,
        .can_write = __vfs_can_always,
        .eof       = __vfs_eof_never,
    },
};

MODULE_INIT(pty, pty_init, NULL)
