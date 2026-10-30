#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <core/system.h>

/*
 * The virtual filesystem (fs/): what every filesystem provides (struct
 * fs, its vnode operations), vnodes and open files, and the operations
 * the rest of the kernel calls on them, whatever filesystem they're on.
 * See docs/kernel/vfs.md.
 */

LOGGER_DECLARE(vfs_log);

struct fs;
struct vnode;
struct file;
struct uio;
struct vops;
struct iobuf;

#include <aquila/dirent.h>

/**
 * An open file's operations. `read` and `write` move the bytes through an
 * iobuf (fs/iobuf.h), whose `resid` is how many are asked for.
 */
struct fops {
    int         (*open)    (struct file *file);
    ssize_t     (*read)    (struct file *file, struct iobuf *io);
    ssize_t     (*write)   (struct file *file, struct iobuf *io);
    ssize_t     (*readdir) (struct file *file, struct dirent *dirent);  
    off_t       (*lseek)   (struct file *file, off_t offset, int whence);
    int         (*close)   (struct file *file);
    int         (*ioctl)   (struct file *file, int request, void *argp);
    int         (*trunc)   (struct file *file, off_t len);

    /* for select() and poll(), and end of file */
    int         (*can_read)  (struct file *file, size_t size);
    int         (*can_write) (struct file *file, size_t size);
    int         (*eof)       (struct file *);

    /* how long a read with nothing to return waits (ns); 0 means until
     * data comes */
    uint64_t    (*read_timeout) (struct file *);
};

#include <aquila/errno.h>
#include <sys/proc.h>
#include <ds/queue.h>
#include <fs/stat.h>
#include <mm/vm.h>

MALLOC_DECLARE(M_VNODE);

/**
 * Who does a filesystem operation: where its paths start, and its
 * credentials and umask.
 */
struct uio {
    struct vnode *root; /* where absolute paths start (NULL: vfs_root) */
    struct vnode *cwd;  /* where relative ones start (NULL: the root) */
    uid_t    uid;
    gid_t    gid;
    mode_t   mask;
    uint32_t flags;
};

/** A filesystem's vnode operations, any of which may be NULL. */
struct vops {
    /* `io->resid` bytes at `offset`, through the iobuf */
    ssize_t (*read)    (struct vnode *vnode, off_t offset, struct iobuf *io);
    ssize_t (*write)   (struct vnode *vnode, off_t offset, struct iobuf *io);
    int     (*ioctl)   (struct vnode *vnode, int request, void *argp);
    int     (*close)   (struct vnode *vnode);
    int     (*trunc)   (struct vnode *vnode, off_t len);

    ssize_t (*readdir) (struct vnode *dir, off_t offset, struct dirent *dirent);
    int     (*finddir) (struct vnode *dir, const char *name, struct dirent *dirent);

    int     (*vmknod)  (struct vnode *dir, const char *fn, uint32_t mode, dev_t dev, struct uio *uio, struct vnode **ref);
    int     (*vunlink) (struct vnode *dir, const char *fn, struct uio *uio);
    int     (*vlink)   (struct vnode *dir, const char *fn, struct vnode *vnode, struct uio *uio);
    int     (*vrename) (struct vnode *odir, const char *ofn, struct vnode *ndir, const char *nfn, struct uio *uio);

    /* stores the vnode's mode, owner and times, after they were changed */
    int     (*setattr) (struct vnode *vnode);

    int     (*vget)    (struct vnode *super, ino_t ino, struct vnode **vnode);

    int     (*vsync)   (struct vnode *vnode, int mode);
    int     (*sync)    (struct vnode *super, int mode);

    int     (*map)     (struct vm_space *vm_space, struct vm_entry *vm_entry);
};

/** A filesystem type. */
struct fs {
    char *name;
    int (*init)  ();
    int (*load)  (struct vnode *dev, struct vnode **super);  /* reads the superblock */
    int (*mount) (struct vnode *dir, int flags, void *data); /* mounts on dir: vfs_bind() */

    struct vops vops;
    struct fops fops;

    /* whether it needs no device (tmpfs, procfs) */
    int nodev;

    /* whether reading its regular files goes through the page cache
     * (fs/vm_object.c): its files are on a disk (ext2, minix) */
    int cached;
};

/** A file in memory (a vnode): what its inode says, and the kernel's own
 * state. */
struct vnode {
    ino_t       ino;
    size_t      size;
    dev_t       dev;
    dev_t       rdev;
    mode_t      mode;
    uid_t       uid;
    gid_t       gid;
    nlink_t     nlink;

    _time_t     atime;
    _time_t     mtime;
    _time_t     ctime;

    struct fs   *fs;

    /** the filesystem's own data */
    void *p;

    /** how many references there are to it */
    size_t ref;

    /** where its readers and writers sleep */
    struct waitq *read_queue;
    struct waitq *write_queue;

    /** its vm object: its pages, read in (the page cache) */
    struct vm_object *vm_object;

    /** the pipe of a FIFO while it is open */
    struct pipe *fifo;

    /** for a directory that a filesystem is mounted on, that filesystem's
     *  root; for a mounted filesystem's root, the directory it's mounted
     *  on */
    struct vnode *mounted;
    struct vnode *covered;
};

/**
 * An open file (POSIX's "open file description", fs/file.c). There's one
 * for each open(), pipe(), socket() or accept(), with its offset and
 * status flags, shared by the descriptors that dup() and fork() copy.
 */
struct file {
    union {
        struct vnode *vnode;
        struct socket *socket;
    };

    off_t offset;
    int flags;      /* open flags (O_*) */

    /* its flock() lock */
    struct file_lock *lock;

    /* how many descriptors, and system calls, refer to it */
    int ref;
};

/* Makes a new open file, whose one reference is the caller's. */
struct file *file_new(void);

/* Adds a reference to an open file. */
struct file *file_ref(struct file *file);

/* Drops a reference to an open file; the last one closes and frees it,
 * and returns the close's result. */
int file_put(struct file *file);

/* Holds an open file until the current thread's system call returns. */
struct file *file_hold(struct file *file);

/** The list of registered filesystem types. */
struct fs_list {
    /** the type's name */
    const char *name;

    /** the type */
    struct fs  *fs;

    /** the next entry of the list */
    struct fs_list *next;
};

extern struct fs_list *registered_fs;
extern struct vnode *vfs_root;

/* can_read, can_write and eof functions for files that always or never
 * can, or are at their end. */
static inline int __vfs_can_always(struct file *f, size_t s){return 1;}
static inline int __vfs_can_never (struct file *f, size_t s){return 0;}
static inline int __vfs_eof_always(struct file *f){return 1;}
static inline int __vfs_eof_never (struct file *f){return 0;}

/* Whether a vnode is a device. */
#define ISDEV(vnode) (S_ISCHR((vnode)->mode) || S_ISBLK((vnode)->mode))

/* Filesystems and mounts */
void    vfs_init(void);
int     vfs_install(struct fs *fs);
int     vfs_mount_root(struct vnode *vnode);
int     vfs_mount_root_dev(const char *name, const char *type);
int     vfs_bind(struct vnode *dir, struct vnode *root);
int     vfs_mount(const char *type, const char *dir, int flags, void *data, struct uio *uio);

/* The vnode operations, whatever the filesystem */
int     vfs_vmknod(struct vnode *dir, const char *fn, uint32_t mode, dev_t dev, struct uio *uio, struct vnode **ref);
int     vfs_vcreat(struct vnode *dir, const char *fn, struct uio *uio, struct vnode **ref);
int     vfs_vmkdir(struct vnode *dir, const char *dname, struct uio *uio, struct vnode **ref);
int     vfs_vunlink(struct vnode *dir, const char *fn, struct uio *uio);
int     vfs_vlink(struct vnode *dir, const char *fn, struct vnode *vnode, struct uio *uio);
int     vfs_vrename(struct vnode *odir, const char *ofn, struct vnode *ndir, const char *nfn, struct uio *uio);
int     vfs_setattr(struct vnode *vnode);
int     vfs_vget(struct vnode *super, ino_t ino, struct vnode **vnode);

int     vfs_map(struct vm_space *vm_space, struct vm_entry *vm_entry);

ssize_t vfs_read_io(struct vnode *vnode, off_t offset, struct iobuf *io);
ssize_t vfs_read_raw(struct vnode *vnode, off_t offset, struct iobuf *io);
ssize_t vfs_write_io(struct vnode *vnode, off_t offset, struct iobuf *io);

/* The kernel's own reads and writes, with a kernel buffer */
ssize_t vfs_read(struct vnode *vnode, off_t offset, size_t size, void *buf);
ssize_t vfs_write(struct vnode *vnode, off_t offset, size_t size, void *buf);
int     vfs_ioctl(struct vnode *vnode, unsigned long request, void *argp);
int     vfs_close(struct vnode *vnode);
int     vfs_trunc(struct vnode *vnode, off_t len);

struct vm_page *vfs_pagein(struct vnode *vnode, off_t offset);

/* Writing cached data to the disks */
#define FS_MSYNC    0x0001
#define FS_DSYNC    0x0002
int vfs_vsync(struct vnode *vnode, int mode);
int vfs_fssync(struct vnode *super, int mode);
int vfs_sync(int mode);

ssize_t vfs_readdir(struct vnode *dir, off_t offset, struct dirent *dirent);
int     vfs_finddir(struct vnode *dir, const char *name, struct dirent *dirent);
int     vfs_dir_empty(struct vnode *dir);

/* The open file operations, whatever the file */
int     vfs_file_open(struct file *file);
ssize_t vfs_file_read(struct file *file, struct iobuf *io);
ssize_t vfs_file_write(struct file *file, struct iobuf *io);
ssize_t vfs_file_readdir(struct file *file, struct dirent *dirent);  
off_t   vfs_file_lseek(struct file *file, off_t offset, int whence);
ssize_t vfs_file_close(struct file *file);
int     vfs_file_flock(struct file *file, int op);
void    vfs_file_lock_release(struct file *file);
int     vfs_file_ioctl(struct file *file, int request, void *argp);
int     vfs_file_trunc(struct file *file, off_t len);

/* For select() and poll(), and end of file */
int     vfs_file_can_read(struct file * file, size_t size);
struct waitq *vfs_file_wait_queue(struct file *file, int write);
int     vfs_file_can_write(struct file * file, size_t size);
int     vfs_file_eof(struct file *);

/* Path lookup (fs/lookup.c) */
int     vfs_lookup(const char *path, struct uio *uio, struct vnode **vnode);
int     vfs_lookup_parent(const char *path, struct uio *uio, struct vnode **dir, char **name);
int     vfs_lookup_dir(const char *path, struct uio *uio, struct vnode **vnode, struct vnode **dir);
int     vfs_parent(struct vnode *dir, struct vnode **parent);
struct vnode *vfs_mount_root_of(struct vnode *vnode);
int     vfs_under(struct vnode *vnode, struct vnode *dir);
int     vfs_vnode_path(struct vnode *vnode, struct vnode *root, char *buf, size_t size);

/* Operations by path */
int     vfs_creat(const char *path, mode_t mode, struct uio *uio, struct vnode **ref);
int     vfs_mkdir(const char *path, mode_t mode, struct uio *uio, struct vnode **ref);
int     vfs_mknod(const char *path, uint32_t mode, dev_t dev, struct uio *uio, struct vnode **ref);
int     vfs_unlink(const char *path, struct uio *uio);
int     vfs_rmdir(const char *path, struct uio *uio);
int     vfs_link(const char *oldpath, struct uio *ouio, const char *newpath, struct uio *nuio);
int     vfs_symlink(const char *target, const char *path, struct uio *uio);
ssize_t vfs_readlink(const char *path, struct uio *uio, char *buf, size_t size);
int     vfs_rename(const char *oldpath, struct uio *ouio, const char *newpath, struct uio *nuio);
int     vfs_stat(struct vnode *vnode, struct aquila_stat *buf);
int     vfs_perms_check(struct file *file, struct uio *uio);

/* The access modes vfs_access() checks */
#define VFS_R   4
#define VFS_W   2
#define VFS_X   1
int     vfs_access(struct vnode *vnode, struct uio *uio, int mode);

/* Changing attributes; `times` is atime then mtime (or UTIME_NOW,
 * UTIME_OMIT) */
int     vfs_chmod(struct vnode *vnode, mode_t mode, struct uio *uio);
int     vfs_chown(struct vnode *vnode, uid_t uid, gid_t gid, struct uio *uio);
int     vfs_utimens(struct vnode *vnode, const struct aquila_timespec times[2], struct uio *uio);

/** A mount's description, as /proc/mounts shows it. */
struct mountpoint {
    const char *dev;
    const char *path;
    const char *type;
    const char *options;
};

#endif /* ! _FS_VFS_H */
