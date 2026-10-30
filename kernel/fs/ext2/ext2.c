/** \file
 * ext2: loading and mounting the filesystem, and the vnodes of its
 * inodes. The block group descriptors are kept in memory, and the vnodes
 * in use in a cache by inode number. See `ext2.h` for the disk's format.
 */
#include <core/panic.h>
#include <core/module.h>
#include <fs/vfs.h>
#include <fs/posix.h>
#include <fs/vcache.h>
#include <aquila/errno.h>
#include <ds/bitmap.h>

#include <ext2.h>

MALLOC_DEFINE(M_EXT2, "ext2", "ext2 filesystem structure");
MALLOC_DEFINE(M_EXT2_SB, "ext2-sb", "ext2 filesystem superblock structure");
MALLOC_DEFINE(M_EXT2_GROUP, "ext2-group", "ext2 block group descriptor");

/**
 * Gets the vnode of an inode: the cached one if it's in use, or a new one
 * made from the inode on the disk.
 *
 * @param desc       the mounted filesystem
 * @param ino        the inode number
 * @param ref_inode  if not NULL, set to the vnode
 * @return 0, `-ENOMEM`, or an I/O error
 */
int ext2_inode_build(struct ext2 *desc, ino_t ino, struct vnode **ref_inode)
{
    int err = 0;
    struct vnode *inode = NULL;

    /* is it in use already? */
    if ((inode = vcache_find(desc->vcache, ino)))
        goto found;

    vfs_log(LOG_DEBUG, "ext2_inode_build(desc=%p, ino=%d, ref_inode=%p)\n", desc, ino, ref_inode);
    
    struct ext2_inode einode;

    /* nothing is cached of an inode that couldn't be read */
    if ((err = ext2_inode_read(desc, ino, &einode)))
        return err;

    inode = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);

    if (!inode)
        return -ENOMEM;

    inode->ino   = ino;
    inode->size  = einode.size;
    inode->mode  = einode.mode;
    inode->uid   = einode.uid;
    inode->gid   = einode.gid;
    inode->nlink = einode.nlink;

    /* a device node's number is in its first block pointer, as Linux
     * keeps it (major << 8 | minor, which is AquilaOS's dev_t); if that's
     * 0, it's in the second, in Linux's newer encoding of larger numbers */
    if (S_ISCHR(einode.mode) || S_ISBLK(einode.mode)) {
        uint32_t old = einode.direct_pointer[0], new = einode.direct_pointer[1];

        inode->rdev = old ? (dev_t) old :
            DEV((new & 0xFFF00) >> 8, (new & 0xFF) | ((new >> 12) & 0xFFF00));
    }

    inode->atime.tv_sec  = einode.atime;
    inode->atime.tv_nsec = 0;
    inode->mtime.tv_sec  = einode.mtime;
    inode->mtime.tv_nsec = 0;
    inode->ctime.tv_sec  = einode.ctime;
    inode->ctime.tv_nsec = 0;

    inode->fs = &ext2fs;
    inode->p = desc;

    vcache_insert(desc->vcache, inode);

found:
    if (ref_inode)
        *ref_inode = inode;

    return 0;
}

/**
 * Writes a vnode's attributes back to its inode on the disk.
 *
 * @param inode  the vnode
 * @return 0, or an I/O error
 */
int ext2_inode_sync(struct vnode *inode)
{
    vfs_log(LOG_DEBUG, "ext2_inode_sync(inode=%p)\n", inode);

    int err = 0;
    
    struct ext2 *desc = (struct ext2 *) inode->p;
    struct ext2_inode ext2_inode;

    /* read the on-disk inode first so that fields we don't track in the
     * vnode (block pointers, flags, ...) are preserved */
    if ((err = ext2_inode_read(desc, inode->ino, &ext2_inode)))
        return err;

    ext2_inode.size   = inode->size;
    ext2_inode.mode   = inode->mode;
    ext2_inode.uid    = inode->uid;
    ext2_inode.gid    = inode->gid;
    ext2_inode.nlink  = inode->nlink;

    ext2_inode.atime = inode->atime.tv_sec;
    ext2_inode.mtime = inode->mtime.tv_sec;
    ext2_inode.ctime = inode->ctime.tv_sec;

    if ((err = ext2_inode_write(desc, inode->ino, &ext2_inode)) < 0)
        return err;

    return 0;
}

/* The filesystem's operations */

/**
 * Installs the ext2 filesystem at boot.
 *
 * @return 0, or `-ENOMEM`
 */
int ext2_init()
{
    return vfs_install(&ext2fs);
}

/**
 * Loads an ext2 filesystem from a device: its superblock, its block group
 * descriptors, and its root directory's vnode.
 *
 * @param dev    the device
 * @param super  if not NULL, set to the root directory's vnode
 * @return 0, `-EINVAL` if the device doesn't have an ext2 filesystem that
 *         AquilaOS can use, or a negative errno
 */
int ext2_load(struct vnode *dev, struct vnode **super)
{
    int err = 0;
    ssize_t r;

    struct ext2_superblock *sb = NULL;
    struct ext2 *desc = NULL;

    sb = kmalloc(sizeof(struct ext2_superblock), &M_EXT2_SB, M_ZERO);
    if (!sb) goto e_nomem;

    /* the superblock: all of it, a short read is an I/O error */
    if ((r = vfs_read(dev, 1024, sizeof(struct ext2_superblock), sb)) != (ssize_t) sizeof(struct ext2_superblock)) {
        err = r < 0 ? (int) r : -EIO;
        goto error;
    }

    /* is it ext2, with blocks of 1, 2 or 4 KiB (the code keeps blocks in
     * buffers, some of them on the stack) and groups of blocks? */
    if (sb->ext2_signature != EXT2_SIGNATURE || sb->block_size > 2 ||
        !sb->gblocks) {
        err = -EINVAL;
        goto error;
    }

    /* the mounted filesystem's structure */
    desc = kmalloc(sizeof(struct ext2), &M_EXT2, M_ZERO);
    if (!desc) goto e_nomem;

    desc->supernode = dev;
    desc->superblock = sb;
    desc->bs = 1024UL << sb->block_size;
    desc->inode_size = EXT2_GOOD_OLD_INODE_SIZE;

    /* revision 1 has an inode size, and features AquilaOS must know */
    if (sb->major_version >= 1) {
        desc->inode_size = sb->inode_size;

        if (desc->inode_size < EXT2_GOOD_OLD_INODE_SIZE || desc->inode_size > desc->bs ||
            (desc->inode_size & (desc->inode_size - 1))) {
            err = -EINVAL;
            goto error;
        }

        desc->filetype = !!(sb->feature_incompat & EXT2_INCOMPAT_FILETYPE);

        uint32_t incompat = sb->feature_incompat & ~EXT2_INCOMPAT_SUPPORTED;
        uint32_t ro = sb->feature_ro_compat & ~EXT2_RO_COMPAT_SUPPORTED;

        if (incompat || ro) {
            printk("ext2: features it can't use: incompat %x, ro_compat %x\n", incompat, ro);
            err = -EINVAL;
            goto error;
        }
    }

    /* the block group descriptor table, after the superblock */
    uint32_t groups    = (desc->bs == 1024)? 2048 : desc->bs;
    size_t   ngroups   = (sb->nblocks + sb->gblocks - 1)/ sb->gblocks;
    size_t   groups_sz = ngroups * sizeof(struct ext2_group);

    desc->ngroups = ngroups;
    desc->groups  = kmalloc(groups_sz, &M_EXT2_GROUP, 0);
    if (!desc->groups) goto e_nomem;

    if ((r = vfs_read(desc->supernode, groups, groups_sz, desc->groups)) != (ssize_t) groups_sz) {
        err = r < 0 ? (int) r : -EIO;
        goto error;
    }

    desc->vcache = kmalloc(sizeof(struct vcache), &M_VCACHE, M_ZERO);
    if (!desc->vcache) goto e_nomem;

    vcache_init(desc->vcache);

    if (!desc->vcache->hashmap) goto e_nomem;

    /* the root directory (if its inode can't be read, there's no
     * filesystem) */
    if (super && (err = ext2_inode_build(desc, 2, super)))
        goto error;

    return 0;

e_nomem:
    err = -ENOMEM;
error:
    /* nothing of it is in use yet, since the root comes last */
    if (desc) {
        if (desc->vcache && desc->vcache->hashmap)
            hashmap_free(desc->vcache->hashmap);

        kfree(desc->vcache);
        kfree(desc->groups);
        kfree(desc);
    }

    kfree(sb);
    return err;
}

/**
 * Mounts the ext2 filesystem of a device on a directory.
 *
 * @param dir    the directory
 * @param flags  not used
 * @param _args  the arguments, whose `dev` is the device's path
 * @return 0, `-EINVAL`, or a negative errno
 */
int ext2_mount(struct vnode *dir, int flags, void *_args)
{
    struct {
        char *dev;
        char *opt;
    } *args = _args;

    struct vnode *dev = NULL;
    int err;

    struct uio uio = {0};   /* the device is looked up as root */

    if (!args || !args->dev)
        return -EINVAL;

    if ((err = vfs_lookup(args->dev, &uio, &dev)))
        goto error;

    struct vnode *fs;

    if ((err = ext2fs.load(dev, &fs)))
        goto error;

    return vfs_bind(dir, fs);

error:
    return err;
}

struct fs ext2fs = {
    .name  = "ext2",
    .init  = ext2_init,
    .load  = ext2_load,
    .mount = ext2_mount,

    .vops = {
        .read    = ext2_read,
        .write   = ext2_write,
        .readdir = ext2_readdir,
        .finddir = ext2_finddir,

        .trunc   = ext2_trunc,
        
        .vmknod  = ext2_vmknod,
        .vunlink = ext2_vunlink,
        .vlink   = ext2_vlink,
        .vrename = ext2_vrename,
        .vget    = ext2_vget,
        .setattr = ext2_setattr,
        .close   = ext2_close,
    },

    .fops = {
        .open    = posix_file_open,
        .read    = posix_file_read,
        .write   = posix_file_write,
        .readdir = posix_file_readdir,
        .lseek   = posix_file_lseek,
        .trunc   = posix_file_trunc,

        .eof     = posix_file_eof,
    },

    /* its files are on a disk: read through the page cache */
    .cached = 1,
};

MODULE_INIT(ext2, ext2_init, NULL)
