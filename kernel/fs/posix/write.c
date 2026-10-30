/** \file
 * The generic write operation for files that are vnodes: regular files,
 * and devices, pipes and FIFOs, which may have to wait for room.
 */

#include <core/system.h>

#include <fs/vfs.h>
#include <fs/iobuf.h>

#include <aquila/fcntl.h>
#include <aquila/errno.h>

/**
 * Writes to an open file at its offset, and moves the offset past what
 * was written. With `O_APPEND`, a regular file is written at its end. A
 * blocking write waits for room (in a pipe, say) until everything is
 * written; a non-blocking one writes only if it can without waiting.
 *
 * @param file  the open file
 * @param io    the data, and how much of it
 * @return the number of bytes written, `-EAGAIN` if a non-blocking write
 *         would wait, or a negative errno if nothing was written
 */
ssize_t posix_file_write(struct file *file, struct iobuf *io)
{
    if (!(file->flags & (O_WRONLY | O_RDWR)))   /* not open for writing */
        return -EBADF;

    /* appending: each write goes at the end, wherever the offset was
     * (POSIX: as if by an lseek() to the end, then the write) */
    if ((file->flags & O_APPEND) && S_ISREG(file->vnode->mode))
        file->offset = file->vnode->size;
    
    if (file->flags & O_NONBLOCK) {
        if (vfs_file_can_write(file, io->resid)) {
            ssize_t retval = vfs_write_io(file->vnode, file->offset, io);

            if (retval < 0)
                return retval;

            file->offset += retval;
            
            /* there's data now: wake up the readers waiting for some */
            if (file->vnode->read_queue)
                waitq_wakeup(file->vnode->read_queue);

            return retval;
        } else {
            /* the write would have to wait */
            return -EAGAIN;
        }
    } else {
        size_t size = io->resid;
        
        while (io->resid) {
            ssize_t r = vfs_write_io(file->vnode, file->offset + (size - io->resid), io);

            if (r < 0) {
                /* report the error only if nothing was written */
                if (io->resid == size)
                    return r;
                break;
            }

            /* done: everything is written, the user buffer faulted, or the
             * end of the file was reached */
            if (!io->resid || io->fault || vfs_file_eof(file))
                break;

            /* readers may be asleep waiting for what was just written
             * (a pipe's ring holds less than one stdio buffer); only if
             * something was: a pipe's writers sleep on the same queue, and
             * two waiting for room would wake each other up for good */
            if (r > 0 && file->vnode->read_queue)
                waitq_wakeup(file->vnode->read_queue);

            /* nothing would wake a waiting writer up: no more fits */
            if (!file->vnode->write_queue)
                break;

            /* wait for room; a signal ends the call, which returns what
             * was written so far, or the error if nothing was */
            int err = waitq_sleep(file->vnode->write_queue);

            if (err) {
                if (io->resid == size)
                    return err;
                break;
            }
        }
        
        ssize_t retval = size - io->resid;

        file->offset += retval;

        /* there's data now: wake up the readers waiting for some */
        if (file->vnode->read_queue)
            waitq_wakeup(file->vnode->read_queue);

        return retval;
    }
}
