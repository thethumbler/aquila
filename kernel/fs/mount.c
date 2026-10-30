/** \file
 * Mounting: a filesystem on a directory, by type (`mount()`), and the
 * root filesystem from a disk partition. `/proc/mounts` lists the mounts.
 */

#include <core/system.h>
#include <core/string.h>
#include <fs/vfs.h>
#include <mm/vm.h>
#include <fs/devfs.h>
#include <dev/dev.h>
#include <ds/queue.h>

/* the mounts, for /proc/mounts */
struct queue *mounts = QUEUE_NEW();

MALLOC_DEFINE(M_MOUNTPOINT, "mountpoint", "mount point structure");

/**
 * Mounts a filesystem of a type on a directory, for `mount()`.
 *
 * @param type   the filesystem's name (`"ext2"`, `"procfs"`, ...)
 * @param dir    the directory's path
 * @param flags  passed on to the filesystem
 * @param data   the filesystem's arguments (its device, its options)
 * @param uio    who is asking
 * @return 0, `-EINVAL` for an unknown type, `-ENOTDIR`, `-EBUSY` if
 *         something is mounted there already, or the filesystem's error
 */
int vfs_mount(const char *type, const char *dir, int flags, void *data, struct uio *uio)
{
    struct fs *fs = NULL;

    /* find the filesystem */
    for (struct fs_list *entry = registered_fs; entry; entry = entry->next) {
        if (!strcmp(entry->name, type)) {
            fs = entry->fs;
            break;
        }
    }

    if (!fs)
        return -EINVAL;

    /* the directory it goes on */
    struct vnode *covered = NULL;
    int err;

    if ((err = vfs_lookup(dir, uio, &covered)))
        return err;

    if (!S_ISDIR(covered->mode)) {
        vfs_close(covered);
        return -ENOTDIR;
    }

    if (covered->mounted) {
        vfs_close(covered);
        return -EBUSY;
    }

    /* where it is, for /proc/mounts */
    char *path = kmalloc(1024, &M_BUFFER, 0);

    if (!path || vfs_vnode_path(covered, NULL, path, 1024))
        err = path ? -ENAMETOOLONG : -ENOMEM;
    else
        err = fs->mount(covered, flags, data);

    /* a filesystem on a disk: the page cache's flusher writes back what's
     * written to its files through shared mappings */
    if (err == 0 && fs->cached)
        vm_cache_flusher_start();

    /* record the mount for /proc/mounts; with no memory for that, the
     * mount stays but isn't listed, and a name that couldn't be copied
     * shows as "?" */
    struct mountpoint *mp;

    if (err == 0 && (mp = kmalloc(sizeof(struct mountpoint), &M_MOUNTPOINT, 0))) {
        struct {
            char *dev;
            char *opt;
        } *args = data;

        const char *dev = (args && args->dev) ? strdup(args->dev) : "none";
        const char *t = strdup(type), *p = strdup(path);

        mp->dev  = dev ? dev : "?";
        mp->type = t ? t : "?";
        mp->path = p ? p : "?";
        mp->options = "";

        if (!enqueue(mounts, mp))
            kfree(mp);
    }

    kfree(path);
    vfs_close(covered);

    return err;
}

/** Gets the device number of an ATA disk partition, by its name
 * (`/dev/hda1` to `/dev/hdd4`). Returns 0, or `-ENODEV` for another name. */
static int root_dev(const char *name, dev_t *dev)
{
    if (strncmp(name, "/dev/hd", 7) || name[7] < 'a' || name[7] > 'd' ||
        name[8] < '1' || name[8] > '4' || name[9])
        return -ENODEV;

    *dev = DEV(3, (name[7] - 'a') * 64 + name[8] - '0');
    return 0;
}

/**
 * Mounts the root filesystem from a disk partition (the `root=/dev/hda2`
 * kernel argument). There are no device nodes yet at boot, so the
 * partition's vnode is made here, as the RAM disk's is.
 *
 * @param name  the partition's name, `/dev/hda1` to `/dev/hdd4`
 * @param type  the filesystem's type (`rootfstype=`), or NULL for ext2
 * @return 0, `-ENODEV` for a bad name, `-EINVAL` for an unknown type, or
 *         the filesystem's error
 */
int vfs_mount_root_dev(const char *name, const char *type)
{
    struct fs *fs = NULL;
    dev_t rdev;
    int err;

    if ((err = root_dev(name, &rdev)))
        return err;

    if (!type)
        type = "ext2";

    for (struct fs_list *entry = registered_fs; entry; entry = entry->next) {
        if (!strcmp(entry->name, type)) {
            fs = entry->fs;
            break;
        }
    }

    if (!fs || !fs->load)
        return -EINVAL;

    struct vnode *dev = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
    if (!dev) return -ENOMEM;

    dev->mode = S_IFBLK;
    dev->rdev = rdev;
    dev->fs   = &devfs;
    dev->ref  = 1;

    struct vnode *root = NULL;

    if ((err = fs->load(dev, &root))) {
        kfree(dev);
        return err;
    }

    printk("kernel: root is %s (%s)\n", name, type);

    if (fs->cached)
        vm_cache_flusher_start();

    return vfs_mount_root(root);
}
