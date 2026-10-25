# The virtual filesystem

A UNIX system has one tree of files, from `/` down, but the files in it
live in very different places: an ext2 partition on a disk, an archive the
boot loader left in memory, a filesystem made of RAM, the kernel's own
tables shown as text (`/proc`), the devices (`/dev`). Programs don't care:
they `open()`, `read()`, `write()` and `close()`, and expect the same
answers everywhere.

The *virtual filesystem* (VFS) is the layer that makes this work. It
gives every file, whatever holds it, the same in-memory object and the
same operations; it walks paths across filesystems; it keeps the open
files and their offsets; and it hands each operation to the filesystem
the file belongs to. The filesystems themselves are in
[Filesystems](filesystems.md).

All of this is kernel design: the same on any machine (see
[The kernel and the machine](architecture.md)). Nothing in `kernel/fs/`
knows it runs on x86. The few places the machine shows through are noted
at the end.

## The textbook design

Every UNIX since the Seventh Edition has had the same four pieces, and
SunOS's *vnode* interface (1985) made them independent of the filesystem:

- an in-memory **inode**, or *vnode*, for each file in use: its type,
  permissions, owner, size, times, and which filesystem it belongs to;
- a **table of operations** each filesystem provides for its vnodes
  (read, write, look a name up in a directory, create, remove, ...);
- **path lookup** (`namei` in the old kernels): a path turned into a
  vnode, one component at a time, crossing into other filesystems at
  their *mount points*;
- the **open file table**: an open file has an offset and flags, and a
  process's *file descriptors* are small numbers that refer to open files.

AquilaOS has exactly these, with the names and shapes they had in the
BSDs.

## Vnodes

A file in use is a `struct vnode` ([`include/fs/vfs.h`](../../kernel/include/fs/vfs.h)):

```c
struct vnode {
    ino_t   ino;            /* its number in its filesystem */
    size_t  size;
    dev_t   dev, rdev;      /* rdev: a device node's device */
    mode_t  mode;           /* type and permissions */
    uid_t   uid; gid_t gid;
    nlink_t nlink;          /* the names it has */
    _time_t atime, mtime, ctime;

    struct fs *fs;          /* its filesystem, and its operations */
    void *p;                /* the filesystem's own data */
    size_t ref;             /* who uses it */

    struct waitq *read_queue, *write_queue;  /* who waits on it */
    struct vm_object *vm_object;             /* its pages, if mapped */
    struct pipe *fifo;                       /* a FIFO's pipe, while open */
    struct vnode *mounted, *covered;         /* mount points */
};
```

The kernel works only with vnodes; how the file is stored is the
filesystem's business, behind `p`. A vnode is *referenced* (`ref`) by
whatever uses it: an open file, a process's current directory, a
mount, a lookup in progress. `vfs_vget()` returns a vnode with one more
reference, `vfs_close()` drops one, and when the last one goes the
filesystem's `close` operation decides what happens to it
([`fs/close.c`](../../kernel/fs/close.c)): a disk filesystem keeps it in
its cache, or frees the inode if the file was removed meanwhile; a
filesystem without a `close` keeps it forever.

Each filesystem keeps its own vnodes: there is no global vnode table.
A disk filesystem finds a vnode by inode number in its **vnode cache**, a
hash table (`struct vcache`, [`fs/vcache.c`](../../kernel/fs/vcache.c)),
so that two lookups of one file get the same vnode. Filesystems that live
in memory don't need one: their vnodes are always there, and the inode
number is the vnode's address.

## Filesystems and their operations

A filesystem is a `struct fs`:

```c
struct fs {
    char *name;                                   /* "ext2", "tmpfs", ... */
    int (*init)  ();
    int (*load)  (struct vnode *dev, struct vnode **super);  /* read it, its root */
    int (*mount) (struct vnode *dir, int flags, void *data); /* mount it on dir */
    struct vops vops;   /* what is done to a vnode */
    struct fops fops;   /* what is done to an open file */
    int nodev;          /* needs no device */
};
```

It has two tables of operations, for two kinds of objects:

- **`struct vops`**, on vnodes: `read` and `write` at an offset,
  `readdir` and `finddir` (a directory's entries, by position and by
  name), `vmknod` (a new entry: file, directory, device node, FIFO,
  symbolic link), `vunlink`, `vlink`, `vrename`, `trunc`, `setattr`
  (store the mode, owner and times), `vget` (a vnode by inode number),
  `ioctl`, `close`, `map` (map the file's pages, for `mmap`), and
  `vsync` and `sync` (which no filesystem has yet).
- **`struct fops`**, on open files: `open`, `read`, `write` and `readdir`
  at the file's offset, `lseek`, `ioctl`, `trunc`, `close`, three
  questions: `can_read`, `can_write` (would it wait?) and `eof`, and
  `read_timeout` (how long a read with nothing to return may wait: a
  terminal's `VTIME`).

An operation a filesystem doesn't have is `NULL`, and the VFS answers
`ENOSYS` (or a sensible default: a file without `lseek` is a pipe,
`ESPIPE`).

The VFS calls them through small wrappers, one per operation:
`vfs_read()`, `vfs_vmknod()`, `vfs_file_read()`, ... in
[`fs/vops.c`](../../kernel/fs/vops.c), [`fs/fops.c`](../../kernel/fs/fops.c),
[`fs/read.c`](../../kernel/fs/read.c), [`fs/write.c`](../../kernel/fs/write.c)
and the others. Each one checks the request, and sends it where it goes:

- a **device node** (character or block) goes to the device layer,
  `kdev_*()`, whatever filesystem holds the node (see
  [Devices and terminals](devices.md));
- a **FIFO** becomes an end of a pipe when it's opened;
- a **socket** (an open file with `FILE_SOCKET`) goes to the socket
  layer (see [Networking](networking.md));
- anything else goes to the vnode's filesystem.

### The POSIX helpers

Most filesystems want the same thing from an open file: read and write
at its offset and move it, wait if there's nothing yet, seek, truncate.
`kernel/fs/posix/` has those once, as `posix_file_read()`,
`posix_file_write()`, `posix_file_lseek()`, ...; a filesystem points its
`fops` at them and only writes its `vops`.

`posix_file_read()` ([`fs/posix/read.c`](../../kernel/fs/posix/read.c))
shows how waiting works for anything that can have no data yet (a pipe,
a terminal): it reads; if nothing came and it isn't the end of the file,
it returns `EAGAIN` with `O_NONBLOCK`, else sleeps on the vnode's
`read_queue` until a writer wakes it, or a signal or a timeout comes
(`sched_wait()`). `posix_file_write()` is the mirror image, on the
`write_queue`. A regular file never waits.

## Moving bytes: the iobuf

A read from a file may end in a user program's buffer (`read()`), or in
the kernel's (a filesystem reading its disk, `execve()` reading a
program). The two can't be treated the same: user memory may not be
mapped yet, or not at all, and must be reached with `copyin()` and
`copyout()` (see [System calls](syscalls.md)).

So bytes move through a `struct iobuf` ([`include/fs/iobuf.h`](../../kernel/include/fs/iobuf.h)),
which says where the memory is and whose it is. Whoever produces the data
calls `iobuf_out()`, whoever consumes it `iobuf_in()`, without knowing
which kind of buffer it is. A fault in user memory ends the transfer:
what moved before it counts, and if nothing did, the call returns
`EFAULT`. The VFS, the devices and the sockets all take an iobuf, and
`vfs_read()`/`vfs_write()` wrap a kernel buffer in one.

Disk filesystems move a file's data a block at a time through a kernel
buffer, then the block's piece in or out of the iobuf.

## Path lookup

`vfs_lookup()` ([`fs/lookup.c`](../../kernel/fs/lookup.c)) turns a path
into a vnode. Where it starts is in the `struct uio` that every VFS call
takes, which also says who is asking:

```c
struct uio {
    struct vnode *root;   /* where absolute paths start, NULL: / */
    struct vnode *cwd;    /* where relative ones do */
    uid_t uid; gid_t gid; /* who asks */
    mode_t mask;          /* the umask */
    uint32_t flags;       /* O_NOFOLLOW, ... */
};
```

The walk goes one component at a time:

- **`.`** is the directory itself;
- **`..`** is the directory's parent, its filesystem's own `..` entry;
  at the root of a mounted filesystem, the parent of the directory it's
  mounted on; at the root, the root;
- **a name** is looked up in the directory (`finddir` gives its inode
  number, `vget` the vnode), which the user must be allowed to search
  (`x`); if a filesystem is mounted on it, that filesystem's root
  replaces it.

A **symbolic link** is followed where it's found (the last component
too, unless `O_NOFOLLOW`, and unless a slash follows it): the walk goes
on from the link's directory with the link's target, then what was left
of the path. At most 8 links are followed (POSIX's `SYMLOOP_MAX`), then
`ELOOP`. A trailing slash wants a directory.

Nothing but the path itself is a string: the walk holds vnodes, not
names. A process's current directory is a vnode, so renaming it doesn't
change it; a path for a vnode is computed only when asked for
(`vfs_vnode_path()`, for `getcwd()` and `/proc/mounts`), by going up
through `..` and finding each directory's name in its parent.

Two variants serve the calls that create or remove an entry:
`vfs_lookup_parent()` stops before the last component and returns its
directory and its name (`mkdir`, `unlink`, `rename`); `vfs_lookup_dir()`
returns the vnode and the directory it was found in.

Permissions are checked on the way, as POSIX says (`vfs_access()`): the
owner's bits for the owner, the group's for the group, the others' for
everyone else; root may do anything except run a file that has no `x`
bit at all. There are no supplementary groups and no ACLs.

## Mounts

Mounting puts a filesystem's root on a directory: from then on the walk
goes into the filesystem instead of the directory. A mount is two
pointers ([`fs/vfs.c`](../../kernel/fs/vfs.c), `vfs_bind()`): the directory's
`mounted` is the filesystem's root, the root's `covered` is the directory.
Going down crosses `mounted`, going up through `..` crosses `covered`.
Mounting on `/` replaces the root.

`vfs_mount()` ([`fs/mount.c`](../../kernel/fs/mount.c)) finds the
filesystem by name among the registered ones (`vfs_install()`, which each
filesystem calls from its `MODULE_INIT` function), looks the directory
up, and calls the filesystem's `mount`. A filesystem on a device looks
the device's node up and reads it (`load`); one that needs no device
binds its root. The mount is remembered for `/proc/mounts`.

The root filesystem is mounted before there is any process (see
[Booting](booting.md)): the initramfs from the boot loader's module, or a
disk partition named by `root=` (`vfs_mount_root_dev()`).

AquilaOS has no `umount`: what's mounted stays mounted until the system
stops.

## Open files and descriptors

`open()` ([`sys/sys_file.c`](../../kernel/sys/sys_file.c), `do_openat()`)
looks the path up (and creates the file with `O_CREAT`), checks the
permissions against the open mode, and makes an open file:

```c
struct file {
    union {
        struct vnode *vnode;
        struct socket *socket;
    };
    off_t offset;
    int flags;                  /* O_* */
    struct file_lock *lock;     /* its flock() */
    int ref;
};
```

This is POSIX's *open file description* ([`fs/file.c`](../../kernel/fs/file.c)):
one per `open()`, `pipe()`, `socket()` or `accept()`, with the offset and
the status flags. A process's descriptors are an array of `struct fd`
(`FDS_COUNT`, 64), each pointing to an open file, with its own
`FD_CLOEXEC` flag. `dup()` and `fork()` copy descriptors, not files: the
copies share the file, and its offset, which is why a shell's children
writing to the same output don't overwrite each other.

An open file counts what refers to it: the descriptors, and the system
calls using it at the moment. A system call *holds* each file it uses
(`file_hold()`) until it returns, so a `close()` by another thread can't
free a file from under a `read()` that's sleeping on it. The last
reference closes the file: the filesystem's or the device's `close`,
once, and the vnode reference goes.

## Waiting on several files: poll and select

`poll()` and `select()` wait until any of several files can be read or
written. Each file says which queue a reader or writer of it would sleep
on (`vfs_file_wait_queue()`: the vnode's `read_queue` or `write_queue`, a
socket's own); the thread registers on all of them (`poll_register()`,
[`sys/waitq.c`](../../kernel/sys/waitq.c)) and sleeps. A wakeup of any
of those queues wakes it to look again. A file that can't say what it
would sleep on registers `NULL`, which any wakeup matches.

## File locks

`flock()` locks a whole file, shared or exclusive, for an open file
([`fs/flock.c`](../../kernel/fs/flock.c)): descriptors that share the
open file share the lock. The locks are advisory (only `flock()` looks at
them), kept in one list, and released with `LOCK_UN` or when the open
file is closed. There are no record locks (`fcntl(F_SETLK)`).

## Mapping files

`mmap()` of a file, and `execve()` loading a program, map the file's
pages into a process. A vnode's pages are kept in its `vm_object`
([`fs/vm_object.c`](../../kernel/fs/vm_object.c)), made on the first
mapping: a page is read the first time a process touches it
(`vnode_page_in()`, called from the page fault handler, see
[Memory](memory.md)), and stays for the next process that maps the same
file. Private mappings (`MAP_PRIVATE`, and a program's segments) get a
copy of a page when they write to it. Shared mappings need the
filesystem's `map` operation, which only devices have (the framebuffer):
`MAP_SHARED` on a regular file isn't supported.

The cached pages are never written back; a `write()` to the file, or a
truncate, updates the cached copies.

## Caching and writing to disk

AquilaOS caches in two places:

- the **vnode caches** of the disk filesystems, above;
- the **block cache** of the disks, under the filesystems
  ([`dev/blkcache.c`](../../kernel/dev/blkcache.c)): 8 MiB of 4 KiB
  blocks, written back later, kept by least recent use. Every read and
  write a filesystem makes to its disk goes through it.

A filesystem itself writes through: when a file grows, its inode and
bitmaps are written to the device at once. Writes reach the disk when the
block cache writes them back: at the cache's next use once a block has
been dirty for five seconds, when half the cache is dirty, on `sync()`,
and before rebooting or powering off. `sync()` is the
block cache's flush (`kdev_sync()`); there is no `fsync()`, and the
VFS's own `vfs_sync()` ([`fs/sync.c`](../../kernel/fs/sync.c)) isn't
implemented.

(`fs/bcache.c` is a smaller cache of a filesystem's own blocks, by
number: minix keeps its indirect zone tables in one.)

## Shortcuts

- No `umount`, no `fsync()`, no record locks, no shared file mappings.
- Vnode caches don't shrink: a disk file's vnode stays until it's
  removed.
- The kernel runs with interrupts off and one CPU in it at a time (see
  [SMP and the kernel lock](smp.md)), so none of these structures has a
  lock of its own; a thread that sleeps (on a disk read, on a pipe) must
  expect the world to have changed when it wakes.

## What is the machine's

Nearly nothing. The VFS is written against the kernel's interfaces only:

- the page size (`PAGE_SIZE`) sizes the pages of a mapped file and
  `/proc`'s reads;
- `copyin()` and `copyout()`, under the iobuf, are the architecture's
  (`arch_copy_user()`, see [The kernel and the machine](architecture.md));
- the root device names `root=` accepts (`/dev/hda1` to `/dev/hdd4`, see
  `root_dev()` in [`fs/mount.c`](../../kernel/fs/mount.c)) are the PC's
  ATA disks;
- the disk filesystems read their on-disk structures as they are in
  memory, which works because the CPUs AquilaOS runs on (x86, aarch64)
  are little-endian, as ext2 and minix are.

## Where it is

| File                                     | Layer  | What                                         |
|------------------------------------------|--------|----------------------------------------------|
| `kernel/include/fs/vfs.h`                | kernel | `struct vnode`, `struct fs`, `vops`, `fops`, `struct file`, `struct uio` |
| `kernel/fs/vfs.c`                        | kernel | registering filesystems, `vfs_bind()`, permissions |
| `kernel/fs/lookup.c`                     | kernel | path lookup, `..`, symbolic links, `vfs_vnode_path()` |
| `kernel/fs/mount.c`                      | kernel | `vfs_mount()`, the root from a disk          |
| `kernel/fs/vops.c`, `read.c`, `write.c`, `readdir.c`, `trunc.c`, `ioctl.c`, `close.c` | kernel | the vnode operations' wrappers |
| `kernel/fs/fops.c`                       | kernel | the open file operations' wrappers, devices, FIFOs, sockets |
| `kernel/fs/mknod.c`, `unlink.c`, `link.c`, `attr.c`, `stat.c` | kernel | creating, removing, linking, renaming, attributes |
| `kernel/fs/file.c`                       | kernel | open files, holding them                     |
| `kernel/fs/posix/`                       | kernel | the POSIX file operations most filesystems use |
| `kernel/fs/iobuf.c`                      | kernel | moving bytes to and from user or kernel memory |
| `kernel/fs/vcache.c`, `bcache.c`         | kernel | the vnode cache, a filesystem's block cache  |
| `kernel/fs/flock.c`                      | kernel | `flock()`                                    |
| `kernel/fs/vm_object.c`                  | kernel | a file's pages                               |
| `kernel/fs/sync.c`                       | kernel | (stubs)                                      |
| `kernel/dev/blkcache.c`                  | kernel | the disks' block cache                       |
| `kernel/sys/sys_file.c`                  | kernel | the file system calls, `do_openat()`         |
| `kernel/sys/waitq.c`                     | kernel | `poll_register()`                            |

## References

- *IEEE Std 1003.1 (POSIX.1)*, the System Interfaces volume: `open()`,
  `read()`, `write()`, `rename()`, `poll()`, `mmap()` (`flock()` is BSD's,
  not POSIX's); the Base Definitions' *Pathname Resolution* (4.13) and
  *File Access Permissions* (4.5).
- S. R. Kleiman, *Vnodes: An Architecture for Multiple File System Types
  in Sun UNIX*, USENIX Summer 1986: where vnodes come from.
- Maurice J. Bach, *The Design of the UNIX Operating System*, chapters 4
  and 5: inodes, `namei`, the file table, mounting.
- McKusick, Neville-Neil and Watson, *The Design and Implementation of
  the FreeBSD Operating System*, chapter 8 (*Local Filesystems*) and the
  VFS chapter: vnodes and their operations as BSD has them.
- *Lions' Commentary on UNIX 6th Edition*, John Lions: `namei` and the
  file table in their first form.
