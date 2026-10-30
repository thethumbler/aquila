/** \file
 * tmpfs: a filesystem in memory, gone when the system stops.
 *
 * Its vnodes are its inodes: each lives in memory as long as it has a
 * name or a reference, and its inode number is its address. A file's
 * data is one buffer, a bit bigger than the file (`tmpfs_cap()`), so that
 * a file written a piece at a time isn't copied at every write. The
 * directories are pseudofs's (`fs/pseudofs/`), which devfs and devpts
 * share.
 */

#include <core/system.h>
#include <core/module.h>
#include <core/string.h>
#include <core/panic.h>
#include <core/time.h>

#include <mm/mm.h>

#include <fs/vfs.h>
#include <fs/iobuf.h>
#include <fs/pseudofs.h>
#include <fs/tmpfs.h>
#include <fs/posix.h>

#include <aquila/errno.h>
#include <aquila/dirent.h>

/** Gets a vnode by its inode number, which is its address. */
static int tmpfs_vget(struct vnode *super, ino_t ino, struct vnode **vnode)
{
    /* the vnode is always in memory */
    struct vnode *node = (struct vnode *) ino;
    if (vnode) *vnode = node;

    return 0;
}

/** Frees a vnode that has neither references nor names left. */
static int tmpfs_close(struct vnode *vnode)
{
    if (!vnode->ref && !vnode->nlink) {
        kfree(vnode->p);
        pseudofs_close(vnode);
    }

    return 0;
}

/** Returns how big a file's buffer is for a file of `size` bytes: the
 * first of 64, 96, 144, ... (half as big again each time) that holds it.
 * Growing it to the next one copies the file, which happens less and less
 * often as the file grows. (It depends on the size alone: the buffer is
 * at least this big, whatever sizes the file had before.) */
static size_t tmpfs_cap(size_t size)
{
    size_t cap = 64;

    while (cap < size && cap < (size_t) -1 / 2)
        cap += cap / 2;

    return cap < size ? size : cap;
}

/** Reads a file's data from its buffer. */
static ssize_t tmpfs_read(struct vnode *node, off_t offset, struct iobuf *io)
{
    if (offset < 0)
        return -EINVAL;

    if ((size_t) offset >= node->size)
        return 0;

    return iobuf_out(io, (char *) node->p + offset, node->size - offset);
}

/** Writes a file's data into its buffer, which grows to hold it. */
static ssize_t tmpfs_write(struct vnode *node, off_t offset, struct iobuf *io)
{
    size_t size = io->resid, old_size = node->size;

    if (offset < 0)
        return -EINVAL;

    /* the end would be past what a size can hold (on 32 bits, an offset
     * near 2 GiB and a few GiB of data) */
    if ((size_t) offset + size < (size_t) offset)
        return -EFBIG;

    size_t end = (size_t) offset + size;

    if (end > node->size) {
        if (!node->p || tmpfs_cap(end) > tmpfs_cap(node->size)) {
            /* a bigger buffer, zeroed, so that a gap between the old size
             * and the offset reads as zeros */
            void *new = kmalloc(tmpfs_cap(end), &M_BUFFER, M_ZERO);
            if (!new)
                return -ENOMEM;

            if (node->p)
                memcpy(new, node->p, node->size);
            kfree(node->p);
            node->p = new;
        } else {
            /* the buffer has room: what is between the old end and the
             * offset may be left from a truncation, it reads as zeros */
            memset((char *) node->p + node->size, 0, end - node->size);
        }

        node->size = end;
    }

    ssize_t r = iobuf_in(io, (char *) node->p + offset, size);
    size_t done = r > 0 ? r : 0;

    /* if a fault stopped the copy, the file ends where the data does */
    if (done < size)
        node->size = MAX(old_size, (size_t) offset + done);

    return r;
}

/** Changes a file's length: its buffer is cut, or grows with zeros. */
static int tmpfs_trunc(struct vnode *vnode, off_t len)
{
    if (!vnode || len < 0)
        return -EINVAL;

    if ((size_t) len == vnode->size)
        return 0;

    if (len == 0) {
        kfree(vnode->p);
        vnode->p    = NULL;
        vnode->size = 0;
        return 0;
    }

    size_t sz = MIN((size_t) len, vnode->size);

    /* within the buffer, as it is: what is past the end now reads as
     * zeros when the file grows into it again (tmpfs_write() clears it) */
    if (vnode->p && tmpfs_cap(len) == tmpfs_cap(vnode->size)) {
        if ((size_t) len > vnode->size)
            memset((char *) vnode->p + vnode->size, 0, len - vnode->size);

        vnode->size = len;
        return 0;
    }

    char *buf = kmalloc(tmpfs_cap(len), &M_BUFFER, 0);

    /* with no memory, the file stays as it was */
    if (!buf)
        return -ENOMEM;

    if (vnode->p)
        memcpy(buf, vnode->p, sz);

    if ((size_t) len > vnode->size)
        memset(buf + vnode->size, 0, len - vnode->size);

    kfree(vnode->p);
    vnode->p    = buf;
    vnode->size = len;
    return 0;
}

/* File operations */

/** Says whether `size` bytes can be read from the file's offset. */
static int tmpfs_file_can_read(struct file *file, size_t size)
{
    if ((size_t) file->offset + size < file->vnode->size)
        return 1;

    return 0;
}

/** Says that a write can always go on: tmpfs has no size limit yet. */
static int tmpfs_file_can_write(struct file *file __unused, size_t size __unused)
{
    return 1;
}

/** Says whether the file's offset is at its end. */
static int tmpfs_file_eof(struct file *file)
{
    return (size_t) file->offset == file->vnode->size;
}

/** Installs tmpfs at boot. */
static int tmpfs_init()
{
    return vfs_install(&tmpfs);
}

/** Mounts a new, empty tmpfs on a directory. The `mode=` option (octal)
 * sets its root's permissions, 0777 otherwise. Returns 0, `-ENOMEM`, or
 * `-EBUSY` if something is mounted on the directory already. */
static int tmpfs_mount(struct vnode *dir, int flags __unused, void *data __unused)
{
    /* a new root directory */
    struct vnode *tmpfs_root = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
    if (!tmpfs_root) return -ENOMEM;

    uint32_t mode = 0777;

    struct mdata {
        char *dev;
        char *opt;
    } *mdata = data;

    if (mdata && mdata->opt) {
        char **tokens = tokenize(mdata->opt, ',');
        for (char **token_p = tokens; token_p && *token_p; ++token_p) {
            char *token = *token_p;
            if (!strncmp(token, "mode=", 5)) {    /* an octal number */
                char *t = token + 5;
                mode = 0;
                while (*t) {
                    mode <<= 3;
                    mode |= (*t++ - '0');
                }
            }
        }

        free_tokens(tokens);
    }

    tmpfs_root->ino   = (vino_t) tmpfs_root;
    tmpfs_root->mode  = S_IFDIR | mode;
    tmpfs_root->size  = 0;
    tmpfs_root->nlink = 2;
    tmpfs_root->fs    = &tmpfs;
    tmpfs_root->p     = NULL;
    tmpfs_root->ref   = 1;

    struct timespec ts;
    gettime(&ts);

    tmpfs_root->ctime = ts;
    tmpfs_root->atime = ts;
    tmpfs_root->mtime = ts;

    /* something is mounted there already: the new root goes */
    int err = vfs_bind(dir, tmpfs_root);

    if (err)
        kfree(tmpfs_root);

    return err;
}

struct fs tmpfs = {
    .name   = "tmpfs",
    .nodev  = 1,
    .init   = tmpfs_init,
    .mount  = tmpfs_mount,

    .vops = {
        .read    = tmpfs_read,
        .write   = tmpfs_write,
        .close   = tmpfs_close,
        .trunc   = tmpfs_trunc,

        .readdir = pseudofs_readdir,
        .finddir = pseudofs_finddir,

        .vmknod  = pseudofs_vmknod,
        .vunlink = pseudofs_vunlink,
        .vlink   = pseudofs_vlink,
        .vrename = pseudofs_vrename,
        .setattr = pseudofs_setattr,
        .vget    = tmpfs_vget,
    },
    
    .fops = {
        .open    = posix_file_open,
        .close   = posix_file_close,
        .read    = posix_file_read,
        .write   = posix_file_write, 
        .ioctl   = posix_file_ioctl,
        .lseek   = posix_file_lseek,
        .readdir = posix_file_readdir,
        .trunc   = posix_file_trunc,

        .can_read  = tmpfs_file_can_read,
        .can_write = tmpfs_file_can_write,
        .eof       = tmpfs_file_eof,
    },
};

MODULE_INIT(tmpfs, tmpfs_init, NULL)
