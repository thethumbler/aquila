/** \file
 * Reading from a vnode: from its device, or through its filesystem; a
 * regular file of a filesystem on a disk, through the page cache.
 */

#include <core/system.h>
#include <fs/vfs.h>
#include <fs/iobuf.h>
#include <dev/dev.h>
#include <mm/vm.h>

/**
 * Reads from a vnode into an iobuf (user or kernel memory). A regular
 * file of a filesystem on a disk (`fs->cached`) is read through the page
 * cache, which keeps its pages for the next read (`vm_cache_read()`).
 *
 * @param vnode  the vnode
 * @param off    where to read from
 * @param io     where the data goes, and how much is wanted
 * @return the number of bytes read, `-EINVAL` for an invalid vnode,
 *         `-ENOSYS` if its filesystem can't read, or another negative
 *         errno
 */
ssize_t vfs_read_io(struct vnode *vnode, off_t off, struct iobuf *io)
{
    vfs_log(LOG_DEBUG, "vfs_read_io(vnode=%p, off=%d, size=%d)\n", vnode, off, io->resid);

    if (vnode && !ISDEV(vnode) && S_ISREG(vnode->mode) && vnode->fs && vnode->fs->cached)
        return vm_cache_read(vnode, off, io);

    return vfs_read_raw(vnode, off, io);
}

/**
 * Reads from a vnode into an iobuf, from its device or its filesystem
 * itself, never the page cache: what the page cache reads its pages
 * with.
 *
 * @param vnode  the vnode
 * @param off    where to read from
 * @param io     where the data goes, and how much is wanted
 * @return the number of bytes read, `-EINVAL` for an invalid vnode,
 *         `-ENOSYS` if its filesystem can't read, or another negative
 *         errno
 */
ssize_t vfs_read_raw(struct vnode *vnode, off_t off, struct iobuf *io)
{
    /* invalid request */
    if (!vnode)
        return -EINVAL;

    /* a device node: the driver reads it */
    if (ISDEV(vnode))
        return kdev_read(&VNODE_DEV(vnode), off, io);

    /* invalid request */
    if (!vnode->fs)
        return -EINVAL;

    /* the filesystem doesn't support it */
    if (!vnode->fs->vops.read)
        return -ENOSYS;

    return vnode->fs->vops.read(vnode, off, io);
}

/**
 * Reads from a vnode into a kernel buffer.
 *
 * @param vnode  the vnode
 * @param off    where to read from
 * @param size   how many bytes to read
 * @param buf    the buffer
 * @return the number of bytes read, or a negative errno
 */
ssize_t vfs_read(struct vnode *vnode, off_t off, size_t size, void *buf)
{
    struct iobuf io;
    iobuf_init(&io, buf, size, 0);
    return vfs_read_io(vnode, off, &io);
}
