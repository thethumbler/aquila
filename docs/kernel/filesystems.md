# Filesystems

A filesystem is one way of keeping files: on a disk, in memory, or made
up on the spot. Behind the VFS (see [The virtual filesystem](vfs.md)),
each one provides the same operations on vnodes and open files; the rest
of the kernel never knows which one it's talking to.

AquilaOS has three kinds:

| Kind                 | Filesystems                                   | Where the files are                       |
|----------------------|-----------------------------------------------|-------------------------------------------|
| **on a device**      | ext2, minix, initramfs (cpio)                 | blocks of a disk, or the boot loader's archive |
| **in memory**        | tmpfs, devfs, devpts                          | kernel memory, gone at shutdown           |
| **made up**          | procfs, pipes                                 | nowhere: the kernel answers each read     |

All of them are kernel design, the same on any machine: they are written
against the VFS and the device layer, and nothing in `kernel/fs/` is
x86's or the PC's (see [The kernel and the machine](architecture.md)).
What is the machine's is under them: the disks they read
([Devices and terminals](devices.md)), and the boot loader's module the
initramfs lives in ([Booting](booting.md)).

## Registering and mounting

A filesystem is a `struct fs` with a name, its `vops` and `fops`, and up
to three functions: `init`, `load` (read a device, return the root's
vnode) and `mount` (mount it on a directory). Its `init` runs at boot as
a built-in module (`MODULE_INIT()`, see [Booting](booting.md)) and
registers it with `vfs_install()`. `/proc/filesystems` lists them;
`nodev` marks those that need no device:

```
	minix
nodev	procfs
	ext2
nodev	devpts
nodev	devfs
nodev	tmpfs
```

The cpio archiver registers differently, with the initramfs
(`initramfs_archiver_register()`), since it's only ever the first root.

`mount -t <type> [<device>] <dir>` comes to `vfs_mount()`, which calls the
filesystem's `mount`: a disk filesystem looks the device's node up and
`load`s it, the others bind their root (`vfs_bind()`). The root
filesystem is chosen at boot ([`fs/mount.c`](../../kernel/fs/mount.c),
[`core/main.c`](../../kernel/core/main.c)): the partition `root=` names,
read by the filesystem `rootfstype=` names (ext2 by default), or else the
initramfs.

What a running system has mounted (`/proc/mounts`): devfs on `/dev` by
the kernel (`kmain()`), the rest by the boot script
(`rootfs/etc/runit/1`):

```
none /dev devfs
none /dev/pts devpts
none /proc procfs
none /run tmpfs
none /tmp tmpfs
none /root tmpfs
none /home/live tmpfs
```

(`/root` and `/home/live` only when the root is the read-only initramfs.)

## In memory: pseudofs

The in-memory filesystems share their directories, from
[`fs/pseudofs/pseudofs.c`](../../kernel/fs/pseudofs/pseudofs.c). A
directory's `p` is a linked list of entries, each a name and a vnode; a
directory's `..` is one of its entries, the parent's vnode, as it would
be on a disk, so `rename()` moves it with the directory. The vnodes are
always in memory, and a vnode's inode number is its address. `vmknod`,
`vunlink`, `vlink`, `vrename`, `readdir`, `finddir`, `setattr` and
`close` are pseudofs's; a file's contents are the filesystem's own.

A file whose last link is removed stays while it's open; the last
`close` frees it.

### tmpfs

[`fs/tmpfs/tmpfs.c`](../../kernel/fs/tmpfs/tmpfs.c): a file is one
buffer of kernel memory (`vnode->p`) the size of the file. A write past
the end allocates a bigger one, zeroed, and copies the file over, so a
hole reads as zeros. Each mount is a new, empty filesystem;
`mount -o mode=<octal>` sets its root's permissions.

There is no size limit: tmpfs takes kernel memory until there's none.

### devfs and devpts

devfs ([`fs/devfs/devfs.c`](../../kernel/fs/devfs/devfs.c)) and devpts
([`fs/devpts/devpts.c`](../../kernel/fs/devpts/devpts.c)) are tmpfs with
one root each, made at boot: every mount shows the same directory. They
hold device nodes, whose reads and writes the VFS sends to the device
layer.

devfs is static. The kernel makes `/dev/console` (in `kmain()`), and the
boot script makes the rest with `mknod` (`/dev/null`, `/dev/tty0`,
`/dev/hda1`, ...), whether the device exists or not. devpts is the
pseudo-terminals' (see [Devices and terminals](devices.md)): opening
`/dev/ptmx` makes `/dev/pts/<n>` (`vfs_vmknod()` on `devpts_root`), closing
the master removes it.

## Made up: procfs

`/proc` ([`fs/procfs/procfs.c`](../../kernel/fs/procfs/procfs.c)) shows
the kernel's state as text. Nothing is stored: a read calls a function
that writes the text into a page and copies out the part asked for. Its
end is the end of what was made, so a read is a snapshot.

| File                  | What                                                    |
|-----------------------|---------------------------------------------------------|
| `meminfo`             | total and free memory, the kernel allocator's use       |
| `buddyinfo`           | free blocks of each size in the buddy allocator         |
| `kvmem`               | the kernel allocator's objects, by type                 |
| `cmdline`             | the kernel's command line                               |
| `rdcmdline`           | the initramfs module's command line                     |
| `version`             | the kernel's version                                    |
| `uptime`              | seconds since boot                                      |
| `interrupts`          | each IRQ's count and route (the architecture's: `arch_interrupts_info()`) |
| `filesystems`         | the registered filesystems                              |
| `mounts`              | what's mounted                                          |
| `devices`             | the registered character and block devices              |
| `<pid>/status`        | a process: its name, state, ids, threads                |
| `<pid>/maps`          | its memory map                                          |
| `<pid>/vmstats`       | its memory objects                                      |
| `self`                | the calling process's directory                         |

The vnodes are made on demand and kept in a vnode cache. A file's inode
number says what it is: the top-level files have negative numbers, a
process's directory and files are numbered from its pid
(`PROC_INO()`).

## Pipes

A pipe ([`fs/pipe.c`](../../kernel/fs/pipe.c)) is a ring buffer of 1 KiB
(`PIPE_BUFLEN`) with a vnode for each end. `pipe()` makes both ends; a
FIFO (a named pipe, `mkfifo`) makes them as it's opened, and a third one
for opening it read-write. Readers and writers sleep on one queue:

- a read with nothing in the ring waits for a writer, unless there's
  none left: then it's the end of the file;
- a write with no reader left fails with `EPIPE` (and the writer gets
  `SIGPIPE`);
- a write that doesn't fit waits for the reader, putting in what fits as
  it goes.

When the last end is closed the pipe goes, and a FIFO loses what was in
it.

## On a device

The disk filesystems read and write their device through the VFS
(`vfs_read()`/`vfs_write()` on the device's vnode), byte offsets on a
block device. Under them, the disks' block cache keeps what was read and
holds what was written (see [The virtual filesystem](vfs.md)). They are
written straightforwardly: an operation reads the inode it needs from the
device each time, and writes what it changes back at once. Only the
vnodes are cached.

### initramfs

The initial RAM disk ([`fs/initramfs/`](../../kernel/fs/initramfs/initramfs.c))
is a `cpio` archive, in the old binary format (`cpio -H bin`, `make
initrd`), loaded by the boot loader as a module. The ramdisk device
([`dev/rd/ramdisk.c`](../../kernel/dev/rd/ramdisk.c)) is that module's
memory, mapped where the boot loader left it (`arch_phys_map()`).

The cpio filesystem ([`fs/initramfs/cpio/cpio.c`](../../kernel/fs/initramfs/cpio/cpio.c))
doesn't unpack it. At boot it reads the archive's headers once and makes a
vnode for each entry, linked into its directory, that says where the
entry's data is in the archive; a read copies from there. It's read-only:
its write operations are `rofs`'s ([`fs/rofs.c`](../../kernel/fs/rofs.c)),
which answer `EROFS`. Owners are all root. A writable place is mounted
over it where one is needed (`/tmp`, `/run`, `/root`).

The format's sizes are 32 bits (two 16-bit halves), so a file in it is
under 4 GiB.

### ext2

ext2 ([`fs/ext2/`](../../kernel/fs/ext2/ext2.c)) is the second extended
filesystem, Linux's from 1993, and the root of the hard disk image (`make
hdd` makes it with `mke2fs -t ext2 -b 1024`). Its layout:

- the **superblock**, at byte 1024: the sizes, the block size (1024 <<
  `block_size`), the free counts;
- the disk is split in **block groups**, each with a **block bitmap**, an
  **inode bitmap** and an **inode table**; the **group descriptors**
  follow the superblock and say where each group's are;
- an **inode** has the file's mode, owner, size, times, links, and 15
  block pointers: 12 to data blocks, one to a *singly indirect* block (a
  block of pointers), one *doubly* indirect, one *triply*. With 1 KiB
  blocks that's files up to 16 GiB, of which AquilaOS uses the 4 GiB its
  32-bit size field holds;
- a **directory** is a file of variable-length entries (inode, entry
  length, name length, type, name), padded to 4 bytes; a removed entry is
  merged into the one before it, or has inode 0.

What AquilaOS does with it:

- **Reading a file** ([`fs/ext2/vops.c`](../../kernel/fs/ext2/vops.c))
  goes block by block: `ext2_inode_block_read()`
  ([`fs/ext2/inode.c`](../../kernel/fs/ext2/inode.c)) finds which slot
  of the inode a block is under and how deep (`block_tree()`), and
  follows the indirect blocks down. A pointer that is 0 is a *hole*:
  the block reads as zeros.
- **Writing** allocates what's missing on the way down: data blocks and
  indirect blocks, from the first group with a free one, the bitmap, the
  group descriptor and the superblock rewritten each time
  (`ext2_block_alloc()`, [`fs/ext2/block.c`](../../kernel/fs/ext2/block.c)).
  A new block is zeroed on the disk.
- **Truncating** frees the blocks past the new end, and indirect blocks
  left empty (`ext2_blocks_free()`, [`fs/ext2/remove.c`](../../kernel/fs/ext2/remove.c)),
  and zeroes the rest of the last block, so the file reads zeros if it
  grows again.
- **Creating** (`ext2_vmknod()`) allocates an inode, writes `.` and `..`
  for a directory, and adds the entry to the directory: into an unused
  entry, into the slack of an entry with room to spare, or into a new
  block.
- **Removing**, **linking** and **renaming** (`ext2_vunlink()`,
  `ext2_vlink()`, `ext2_vrename()`) change entries and link counts. A
  file whose last link is removed stays while it's open: its inode and
  blocks are freed when its vnode is closed for the last time
  (`ext2_close()`).
- **Symbolic links** under 60 bytes are *fast*: their target is kept in
  the inode's block pointers, as Linux does.
- **I/O errors** come back: every read and write of the device is checked
  (`ext2_io()`: all of it, or `EIO`), and an operation that can't read a
  bitmap or a directory block doesn't write it back.

`e2fsck` is the test: the `ext2` test writes, truncates and removes files
on a disk image, then has `e2fsck` check it.

Revision 1's feature flags are checked: only `filetype`,
`sparse_super` and `large_file` (what `mke2fs -t ext2` uses) are
accepted, and any other refuses the mount. A device node's number is
kept in its first block pointer, as Linux keeps it.

What it leaves out: ext3's journal, extents and htree directories, files
over 4 GiB, and extended attributes. The superblock's mount count and
state aren't updated.

### minix

The MINIX filesystem ([`fs/minix/`](../../kernel/fs/minix/minix.c)) was
Linux's first, and is the simplest real one: a superblock, an inode
bitmap, a zone (block) bitmap, an inode table, then the data. AquilaOS
reads **version 1** (14- or 30-character names, 16-bit zone numbers); the
version 2 and 3 magic numbers are defined but not accepted.

A minix inode has 9 zone pointers: 7 direct, one singly indirect, one
doubly. The code mirrors ext2's ([`fs/minix/inode.c`](../../kernel/fs/minix/inode.c)):
`zone_tree()` finds where a zone is, holes read as zeros, writing
allocates zones and indirect tables, truncating frees them. The
indirect tables are cached by zone number in a `bcache`. Both bitmaps
are read at mount and kept in memory; each allocation writes the whole
bitmap back.

minix can read, write, create and truncate files and make directories,
but has no `unlink`, `link`, `rename`, `rmdir` or `setattr`: those return
`ENOSYS`. The `minix` test checks what it writes with `fsck.minix`.

## Shortcuts

- tmpfs has no size limit; a growing file is copied whole each time it
  grows.
- devfs is static: device nodes are made by the boot script, not by
  the drivers.
- ext2 and minix write their metadata through at once (bitmaps, group
  descriptors, superblock) rather than batching it; the block cache under
  them is what makes it bearable.
- minix has no `unlink`, `rename`, `rmdir`, `link` or `setattr`.
- A disk filesystem's vnodes stay cached until the file is removed.

## Where it is

| File                                   | Layer  | What                                           |
|----------------------------------------|--------|------------------------------------------------|
| `kernel/fs/mount.c`                    | kernel | mounting, the root from a disk                 |
| `kernel/fs/pseudofs/pseudofs.c`        | kernel | in-memory directories                          |
| `kernel/fs/tmpfs/tmpfs.c`              | kernel | tmpfs                                          |
| `kernel/fs/devfs/devfs.c`              | kernel | devfs                                          |
| `kernel/fs/devpts/devpts.c`            | kernel | devpts                                         |
| `kernel/fs/procfs/procfs.c`            | kernel | `/proc`                                        |
| `kernel/fs/pipe.c`                     | kernel | pipes and FIFOs                                |
| `kernel/fs/initramfs/initramfs.c`      | kernel | the initramfs, its archivers                   |
| `kernel/fs/initramfs/cpio/cpio.c`      | kernel | the cpio archive as a filesystem               |
| `kernel/fs/rofs.c`                     | kernel | the write operations of read-only filesystems  |
| `kernel/fs/ext2/`                      | kernel | ext2: `ext2.c` (load, mount), `inode.c` (inodes, block trees), `block.c` (allocation), `dentry.c` (directories), `vops.c` (read, write, create), `remove.c` (unlink, link, rename) |
| `kernel/fs/minix/`                     | kernel | minix v1, laid out as ext2                     |
| `kernel/dev/rd/ramdisk.c`              | kernel | the ramdisk device: the boot loader's module   |
| `rootfs/etc/runit/1`                   | user   | the boot script: `/dev`'s nodes, the mounts    |

## References

- Dave Poirier, *The Second Extended File System: Internal Layout*
  (<https://www.nongnu.org/ext2-doc/>): the superblock, groups, inodes,
  directories, fast symbolic links.
- Rémy Card, Theodore Ts'o and Stephen Tweedie, *Design and
  Implementation of the Second Extended Filesystem*, First Dutch
  International Symposium on Linux, 1994.
- Andrew S. Tanenbaum and Albert S. Woodhull, *Operating Systems: Design
  and Implementation*, chapter 5: the MINIX filesystem, its bitmaps,
  inodes and zones.
- `cpio(5)` (the old binary format's header), and the `cpio` command's
  `-H bin`.
- *IEEE Std 1003.1 (POSIX.1)*: `pipe()`, `mkfifo()`, and what reads and
  writes on pipes do (`read()`, `write()`).
- The Linux kernel's `Documentation/filesystems/proc.rst` and `tmpfs.rst`:
  the filesystems AquilaOS's are modelled on.
