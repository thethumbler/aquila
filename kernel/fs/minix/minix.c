/** \file
 * minix: loading and mounting the filesystem, and the vnodes of its
 * inodes. The inode and zone bitmaps are kept in memory, and the vnodes in
 * use in a cache by inode number. See `minix.h` for the disk's format.
 */
#include <core/system.h>
#include <core/module.h>

#include <fs/vfs.h>
#include <fs/posix.h>
#include <fs/vcache.h>
#include <fs/bcache.h>
#include <aquila/errno.h>
#include <ds/bitmap.h>

#include <minix.h>

MALLOC_DEFINE(M_MINIX, "minix", "minix filesystem structure");
MALLOC_DEFINE(M_MINIX_SB, "minix-sb", "minix filesystem superblock structure");

/**
 * Gets the vnode of an inode: the cached one if it's in use, or a new one
 * made from the inode on the disk.
 *
 * @param desc  the mounted filesystem
 * @param ino   the inode number
 * @param ref   if not NULL, set to the vnode
 * @return 0, `-ENOMEM`, or an I/O error
 */
int minix_inode_build(struct minix *desc, ino_t ino, struct vnode **ref)
{
    int err = 0;
    struct vnode *inode = NULL;

    /* is it in use already? */
    if ((inode = vcache_find(desc->vcache, ino)))
        goto found;

    inode = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
    if (!inode) return -ENOMEM;

    struct minix_inode minix_inode;
    
    if ((err = minix_inode_read(desc, ino, &minix_inode)))
        goto error;

    inode->ino   = ino;
    inode->size  = minix_inode.size;
    inode->mode  = minix_inode.mode;
    inode->uid   = minix_inode.uid;
    inode->gid   = minix_inode.gid;
    inode->nlink = minix_inode.nlinks;

    /* a device node's number is in its first zone, as Linux keeps it
     * (major << 8 | minor, AquilaOS's dev_t) */
    if (S_ISCHR(minix_inode.mode) || S_ISBLK(minix_inode.mode))
        inode->rdev = (dev_t) minix_inode.zones[0];

    /* version 1 inodes have one time, the modification time */
    inode->mtime.tv_sec  = minix_inode.mtime;
    inode->mtime.tv_nsec = 0;

    inode->fs = &minixfs;
    inode->p  = desc;

    vcache_insert(desc->vcache, inode);

found:
    if (ref)
        *ref = inode;

    return 0;

error:
    if (inode)
        kfree(inode);

    return err;
}

/**
 * Writes a vnode's attributes back to its inode on the disk.
 *
 * @param inode  the vnode
 * @return 0, or an I/O error
 */
int minix_inode_sync(struct vnode *inode)
{
    int err = 0;

    struct minix *desc = inode->p;
    struct minix_inode minix_inode;
    
    if ((err = minix_inode_read(desc, inode->ino, &minix_inode)))
        return err;

    minix_inode.size   = inode->size;
    minix_inode.mode   = inode->mode;
    minix_inode.uid    = inode->uid;
    minix_inode.gid    = inode->gid;
    minix_inode.nlinks = inode->nlink;
    minix_inode.mtime  = inode->mtime.tv_sec;

    return minix_inode_write(desc, inode->ino, &minix_inode);
}

/* The filesystem's operations */

/** Installs the minix filesystem at boot. */
static int minix_init()
{
    return vfs_install(&minixfs);
}

/** Loads a minix filesystem from a device: its superblock, its bitmaps,
 * and its root directory's vnode in `*super`. Returns 0, `-EINVAL` if the
 * device doesn't have a minix v1 filesystem with 1 KiB blocks, or a
 * negative errno. */
static int minix_load(struct vnode *dev, struct vnode **super)
{
    int err = 0;

    struct minix_superblock *sb = NULL;
    struct minix *desc = NULL;
   
    sb = kmalloc(sizeof(struct minix_superblock), &M_MINIX_SB, 0);
    if (sb == NULL) return -ENOMEM;

    if ((err = vfs_read(dev, 1024, sizeof(*sb), sb)) < 0)
        goto error;

    int version = 0;
    size_t name_len = 0;

    switch (sb->magic) {
        case MINIX_MAGIC:
            version  = 1;
            name_len = 14;
            break;
        case MINIX_MAGIC2:
            version  = 1;
            name_len = 30;
            break;
    }

    /* only zones of one 1 KiB block, which version 1 has (the code keeps
     * blocks in buffers on the stack) */
    if (!version || sb->block_size) {
        err = -EINVAL;
        goto error;
    }

    /* the mounted filesystem's structure */
    desc = kmalloc(sizeof(struct minix), &M_MINIX, M_ZERO);
    if (!desc) goto e_nomem;

    desc->supernode   = dev;
    desc->superblock  = sb;

    if (version == 1) {
        desc->bs          = 1024UL << sb->block_size;

        desc->imap_off    = 1024UL + desc->bs;
        desc->zmap_off    = 1024UL + (1 + sb->imap_blocks) * desc->bs;

        desc->inode_table = 1024UL + (1 + sb->imap_blocks + sb->zmap_blocks) * desc->bs;

        desc->inode_size  = 32;
        desc->name_len    = name_len;
        desc->dentry_size = name_len + 2;
    } else {
        err = -ENOTSUP;
        goto error;
    }

    /* the bitmaps are kept in memory: unless all of them can be read,
     * there's no filesystem */
    size_t imap_size = sb->imap_blocks * desc->bs, zmap_size = sb->zmap_blocks * desc->bs;

    desc->imap.map = kmalloc(imap_size, &M_BUFFER, 0);
    desc->zmap.map = kmalloc(zmap_size, &M_BUFFER, 0);
    if (!desc->imap.map || !desc->zmap.map) goto e_nomem;

    /* the bitmaps' last bits: bit i of the inode bitmap is inode i (1 to
     * ninodes), bit i of the zone bitmap is zone firstdatazone + i - 1
     * (zones run to nzones - 1); bit 0 of each is reserved */
    desc->imap.max_idx = sb->ninodes;
    desc->zmap.max_idx = sb->nzones > sb->firstdatazone ? sb->nzones - sb->firstdatazone : 0;

    ssize_t r;

    if ((r = vfs_read(desc->supernode, desc->imap_off, imap_size, desc->imap.map)) != (ssize_t) imap_size ||
        (r = vfs_read(desc->supernode, desc->zmap_off, zmap_size, desc->zmap.map)) != (ssize_t) zmap_size) {
        err = r < 0 ? (int) r : -EIO;
        goto error;
    }

    desc->vcache = kmalloc(sizeof(struct vcache), &M_VCACHE, M_ZERO);
    desc->bcache = kmalloc(sizeof(struct bcache), &M_BCACHE, M_ZERO);
    if (!desc->vcache || !desc->bcache) goto e_nomem;

    vcache_init(desc->vcache);
    bcache_init(desc->bcache);
    if (!desc->vcache->hashmap || !desc->bcache->hashmap) goto e_nomem;

    /* the root directory (if its inode can't be read, there's no
     * filesystem) */
    if (super && (err = minix_inode_build(desc, 1, super)))
        goto error;

    return 0;

e_nomem:
    err = -ENOMEM;
error:
    /* nothing of it is in use yet, since the root comes last */
    if (desc) {
        if (desc->vcache && desc->vcache->hashmap)
            hashmap_free(desc->vcache->hashmap);
        if (desc->bcache && desc->bcache->hashmap)
            hashmap_free(desc->bcache->hashmap);

        kfree(desc->vcache);
        kfree(desc->bcache);
        kfree(desc->imap.map);
        kfree(desc->zmap.map);
        kfree(desc);
    }

    kfree(sb);
    return err;
}

/** Mounts the minix filesystem of a device (the `dev` argument) on a
 * directory. */
static int minix_mount(struct vnode *dir, int flags, void *_args)
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

    if ((err= vfs_lookup(args->dev, &uio, &dev)))
        goto error;

    struct vnode *fs;

    if ((err = minixfs.load(dev, &fs)))
        goto error;

    return vfs_bind(dir, fs);

error:
    return err;
}

struct fs minixfs = {
    .name  = "minix",
    .init  = minix_init,
    .load  = minix_load,
    .mount = minix_mount,

    .vops = {
        .read    = minix_read,
        .write   = minix_write,
        .readdir = minix_readdir,
        .finddir = minix_finddir,
        .trunc   = minix_trunc,
        
        .vmknod  = minix_vmknod,
        .vget    = minix_vget,
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

MODULE_INIT(minix, minix_init, NULL)
