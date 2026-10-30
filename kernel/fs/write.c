/** \file
 * Writing to a vnode: to its device, or through its filesystem.
 */

#include <core/system.h>
#include <fs/vfs.h>
#include <fs/iobuf.h>
#include <dev/dev.h>
#include <mm/vm.h>

/**
 * Writes to a vnode from an iobuf (user or kernel memory). The file's
 * pages in the page cache are refreshed: the bytes written are read
 * again into them.
 *
 * @param vnode  the vnode
 * @param off    where to write
 * @param io     the data, and how much of it
 * @return the number of bytes written, `-EINVAL` for an invalid vnode,
 *         `-ENOSYS` if its filesystem can't write, or another negative
 *         errno
 */
ssize_t vfs_write_io(struct vnode *vnode, off_t off, struct iobuf *io)
{
    vfs_log(LOG_DEBUG, "vfs_write_io(vnode=%p, off=%d, size=%d)\n", vnode, off, io->resid);

    ssize_t ret = vfs_write_raw(vnode, off, io);

    /* refresh the file's pages in the page cache */
    if (ret > 0 && vnode && !ISDEV(vnode))
        vnode_pages_update(vnode, off, off + ret);

    return ret;
}

/**
 * Writes to a vnode from an iobuf, to its device or through its
 * filesystem, and nothing else: the page cache writes its pages back to
 * the file with it (the pages are what's written).
 *
 * @param vnode  the vnode
 * @param off    where to write
 * @param io     the data, and how much of it
 * @return the number of bytes written, `-EINVAL` for an invalid vnode,
 *         `-ENOSYS` if its filesystem can't write, or another negative
 *         errno
 */
ssize_t vfs_write_raw(struct vnode *vnode, off_t off, struct iobuf *io)
{
    /* invalid request */
    if (!vnode)
        return -EINVAL;

    /* a device node: the driver writes it */
    if (ISDEV(vnode))
        return kdev_write(&VNODE_DEV(vnode), off, io);

    /* invalid request */
    if (!vnode->fs)
        return -EINVAL;

    /* the filesystem doesn't support it */
    if (!vnode->fs->vops.write)
        return -ENOSYS;

    return vnode->fs->vops.write(vnode, off, io);
}

/**
 * Writes to a vnode from a kernel buffer.
 *
 * @param vnode  the vnode
 * @param off    where to write
 * @param size   how many bytes to write
 * @param buf    the buffer
 * @return the number of bytes written, or a negative errno
 */
ssize_t vfs_write(struct vnode *vnode, off_t off, size_t size, void *buf)
{
    struct iobuf io;
    iobuf_init(&io, buf, size, 0);
    return vfs_write_io(vnode, off, &io);
}
