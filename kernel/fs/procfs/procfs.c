/** \file
 * procfs: the kernel's and the processes' state as files, usually
 * mounted on `/proc`.
 *
 * Its files hold no data: reading one makes its text then, a snapshot.
 * The top directory has the system's files (`meminfo`, `mounts`, ...),
 * `self`, and a directory for each process, named by its ID, with the
 * process's files (`status`, `maps`, `vmstats`). Vnodes are made when
 * they are looked up, and their inode numbers say what they are: a
 * system file's is negative, and a process's directory and its files
 * have consecutive numbers from the process's ID.
 */

#include <core/system.h>
#include <core/arch.h>
#include <core/module.h>
#include <core/string.h>
#include <core/time.h>
#include <aquila/errno.h>
#include <boot/boot.h>

#include <fs/vfs.h>
#include <fs/iobuf.h>
#include <fs/devfs.h>
#include <fs/posix.h>
#include <fs/vcache.h>

#include <sys/proc.h>
#include <sys/sched.h>

#include <mm/vm.h>
#include <mm/buddy.h>
#include <ds/buddy.h>

struct fs procfs;

/** A file of the top directory: its name, and what makes its text. */
struct procfs_entry {
    char *name;
    ssize_t (*read)(off_t off, size_t size, char *buf);
};

/* procfs's root directory, usually mounted on /proc */
struct vnode *procfs_root = NULL;
struct vcache procfs_vcache = {0};

/** Makes `/proc/meminfo`: the memory there is, and how it's used. */
static ssize_t procfs_meminfo(off_t off, size_t size, char *buf)
{
    char meminfo_buf[512];
    extern size_t k_total_mem, k_used_mem, kvmem_used, kvmem_obj_cnt, slab_used;

    int sz = snprintf(meminfo_buf, 512, 
            "MemTotal: %d kB\n"
            "MemFree: %d kB\n"
            "KVMemUsed: %d KB\n"
            "KVMemObjCnt: %d\n"
            "Slab: %d kB\n",
            (int) (k_total_mem/1024),
            (int) ((k_total_mem-k_used_mem)/1024),
            (int) (kvmem_used/1024),
            (int) kvmem_obj_cnt,
            (int) (slab_used/1024)
            );

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, meminfo_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/kvmem`: the kernel's allocations, by malloc type. */
static ssize_t procfs_kvmem(off_t off, size_t size, char *buf)
{
    extern struct queue *malloc_types;

    char kvmem_buf[1024];
    int sz = 0;

    queue_for (node, malloc_types) {
        struct malloc_type *type = (struct malloc_type *) node->value;
        sz += snprintf(kvmem_buf + sz, sizeof(kvmem_buf) - sz,
                "%s %d %d\n", type->name, type->nr, type->total);

        if ((size_t) sz >= sizeof(kvmem_buf))
            break;
    }

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, kvmem_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/version`: the kernel's name and version. */
static ssize_t procfs_version(off_t off, size_t size, char *buf)
{
    char version_buf[512];

    int sz = snprintf(version_buf, 512,
            "%s version %s (%s version %s) %s\n",
            UTSNAME_SYSNAME, UTSNAME_RELEASE,
            __CONFIG_COMPILER__,
            __CONFIG_COMPILER_VERSION__,
            __CONFIG_TIMESTAMP__
            );

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, version_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/uptime`: the time since boot. */
static ssize_t procfs_uptime(off_t off, size_t size, char *buf)
{
    char uptime_buf[64];

    int sz = snprintf(uptime_buf, 64, "%ld\n", arch_rtime_ms() / 1000);

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, uptime_buf + off, ret);
        return ret;
    }

    return 0;
}

/* the architecture's description of its interrupts, if it has one */
int arch_interrupts_info(char *buf, size_t size) __attribute__((weak));

/** Makes `/proc/interrupts`: the interrupt controller and the IRQs, as the
 * architecture describes them. */
static ssize_t procfs_interrupts(off_t off, size_t size, char *buf)
{
    char info[2048];
    int sz = arch_interrupts_info ? arch_interrupts_info(info, sizeof(info)) : 0;

    sz = MIN(sz, (int) sizeof(info) - 1);

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, info + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/filesystems`: the installed filesystems. */
static ssize_t procfs_filesystems(off_t off, size_t size, char *buf)
{
    char fs_buf[512];

    extern struct fs_list *registered_fs;

    int sz = 0;

    for (struct fs_list *fs = registered_fs; fs; fs = fs->next) {
        sz += snprintf(fs_buf + sz, sizeof(fs_buf) - sz, "%s\t%s\n", fs->fs->nodev? "nodev" : "", fs->name);
        if ((size_t) sz >= sizeof(fs_buf))
            break;
    }

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, fs_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/cmdline`: the kernel's command line. */
static ssize_t procfs_cmdline(off_t off, size_t size, char *buf)
{
    char cmdline_buf[512];

    extern struct boot *__kboot;

    int sz = snprintf(cmdline_buf, 512, "%s\n", __kboot->cmdline);

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, cmdline_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/rdcmdline`: the RAM disk module's command line. */
static ssize_t procfs_rdcmdline(off_t off, size_t size, char *buf)
{
    char rdcmdline_buf[512];

    extern struct boot *__kboot;

    /* none, from a disk (root=): an empty line */
    char *cmdline = __kboot->modules_count ? __kboot->modules[0].cmdline : "";

    int sz = snprintf(rdcmdline_buf, 512, "%s\n", cmdline);

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, rdcmdline_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/mounts`: the mounted filesystems. */
static ssize_t procfs_mounts(off_t off, size_t size, char *buf)
{
    char mounts_buf[4096];

    extern struct queue *mounts;
    int sz = 0;

    queue_for (node, mounts) {
        struct mountpoint *mp = node->value;
        sz += snprintf(mounts_buf + sz, sizeof(mounts_buf) - sz,
                "%s %s %s %s\n", mp->dev, mp->path, mp->type, mp->options);
        if ((size_t) sz >= sizeof(mounts_buf))
            break;
    }


    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, mounts_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/buddyinfo`: the buddy allocator's free blocks, by size. */
static ssize_t procfs_buddyinfo(off_t off, size_t size, char *buf)
{
    char buddyinfo_buf[4096];

    extern struct buddy buddies[BUDDY_ZONE_NR][BUDDY_MAX_ORDER+1];

    int sz = 0;

    sz += snprintf(buddyinfo_buf + sz, sizeof(buddyinfo_buf) - sz, "Zone DMA\n");
    for (int i = 0; i < BUDDY_MAX_ORDER + 1; ++i) {
        sz += snprintf(buddyinfo_buf + sz, sizeof(buddyinfo_buf) - sz, "2^%d: %d\n",
                12 + i, buddies[0][i].usable);
    }

    sz += snprintf(buddyinfo_buf + sz, sizeof(buddyinfo_buf) - sz, "\nZone Normal\n");
    for (int i = 0; i < BUDDY_MAX_ORDER + 1; ++i) {
        sz += snprintf(buddyinfo_buf + sz, sizeof(buddyinfo_buf) - sz, "2^%d: %d\n",
                12 + i, buddies[1][i].usable);
    }

    sz += snprintf(buddyinfo_buf + sz, sizeof(buddyinfo_buf) - sz, "\n");


    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, buddyinfo_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes `/proc/devices`: the registered device drivers. */
static ssize_t procfs_devices(off_t off, size_t size, char *buf)
{
    char dev_buf[512];

    int sz = 0;

    extern struct dev *chrdev[256];
    extern struct dev *blkdev[256];

    sz += snprintf(dev_buf + sz, sizeof(dev_buf) - sz, "chrdev:\n");
    for (int i = 0; i < 256; ++i) {
        if (chrdev[i]) {
            sz += snprintf(dev_buf + sz, sizeof(dev_buf) - sz, "%d\t%s\n", i, chrdev[i]->name);
            if ((size_t) sz >= sizeof(dev_buf))
                break;
        }
    }

    sz += snprintf(dev_buf + sz, sizeof(dev_buf) - sz, "\nblkdev:\n");
    for (int i = 0; i < 256; ++i) {
        if (blkdev[i]) {
            sz += snprintf(dev_buf + sz, sizeof(dev_buf) - sz, "%d\t%s\n", i, blkdev[i]->name);
            if ((size_t) sz >= sizeof(dev_buf))
                break;
        }
    }

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, dev_buf + off, ret);
        return ret;
    }

    return 0;
}

/* the top directory's files */
static struct procfs_entry entries[] = {
    {"meminfo", procfs_meminfo},
    {"cmdline", procfs_cmdline},
    {"rdcmdline", procfs_rdcmdline},
    {"version", procfs_version},
    {"uptime",  procfs_uptime},
    {"interrupts", procfs_interrupts},
    {"filesystems",  procfs_filesystems},
    {"buddyinfo", procfs_buddyinfo},
    {"kvmem", procfs_kvmem},
    {"mounts", procfs_mounts},
    {"devices", procfs_devices},
};

#define PROCFS_ENTRIES  (sizeof(entries)/sizeof(entries[0]))

/** Makes a process's `status`: its IDs, its state and its threads'. */
static ssize_t procfs_proc_status(int pid, off_t off, size_t size, void *buf)
{
    struct proc *proc = proc_pid_find(pid);

    if (!proc)
        return -ESRCH;

    char status_buf[1024], cwd[256];

    if (!proc->cwd || vfs_vnode_path(proc->cwd, NULL, cwd, sizeof(cwd)))
        strcpy(cwd, proc->cwd ? "?" : "/");

    int sz = snprintf(status_buf, sizeof(status_buf),
            "name: %s\n"
            "pid: %d\n"
            "ppid: %d\n"
            "cwd: %s\n"
            "umask: %x\n"
            "uid: %d\n"
            "gid: %d\n"
            "heap: 0x%x\n"
            "threads_nr: %d\n"
            "sigpending: 0x%x\n",
            proc->name,
            proc->pid,
            proc->parent ? proc->parent->pid : 0,
            cwd,
            proc->mask,
            proc->uid,
            proc->gid,
            proc->heap,
            proc->threads.count,
            (unsigned) proc->sig_pending
            );

    /* what each thread is doing: running (or ready to), sleeping (ISLEEP,
     * signals wake it) or waiting (USLEEP), and with a deadline; and its
     * signal mask and own pending signals */
    static const char *states[] = {"running", "sleeping", "waiting", "zombie"};

    queue_for (node, &proc->threads) {
        struct thread *thread = node->value;
        const char *state = thread->state < sizeof(states) / sizeof(states[0]) ?
                            states[thread->state] : "?";

        sz += snprintf(status_buf + sz, sizeof(status_buf) - sz,
                "thread %d%s%s: %s%s%s%s, sigmask 0x%x, sigpending 0x%x\n", thread->tid,
                thread->name ? " " : "", thread->name ? thread->name : "", state,
                thread->waitq ? " on a queue" : "",
                list_linked(&thread->timer_link) ? " with a deadline" : "",
                list_linked(&thread->run_link) && !thread->waitq ? " (ready)" :
                    thread == curthread ? " (current)" : "",
                (unsigned) thread->sigmask, (unsigned) thread->sig_pending);
    }

    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, status_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes a process's `maps`: its mappings, as Linux's `/proc/<pid>/maps`
 * shows them. */
static ssize_t procfs_proc_maps(int pid, off_t off, size_t size, void *buf)
{
    struct proc *proc = proc_pid_find(pid);

    if (!proc)
        return -ESRCH;

    char maps_buf[4096];

    int sz = 0;

    struct queue *vm_entries = &proc->vm_space.vm_entries;
    queue_for (node, vm_entries) {
        struct vm_entry *vm_entry = node->value;

        char perm[5] = {0};
        perm[0] = vm_entry->flags & VM_UR? 'r' : '-';
        perm[1] = vm_entry->flags & VM_UW? 'w' : '-';
        perm[2] = vm_entry->flags & VM_UX? 'x' : '-';
        perm[3] = vm_entry->flags & VM_SHARED? 's' : 'p';

        char *desc = "";

        if (vm_entry == proc->stack_vm)
            desc = "[stack]";

        if (vm_entry == proc->heap_vm)
            desc = "[heap]";

        struct vnode *vnode = NULL;

        if (vm_entry->vm_object && vm_entry->vm_object->type == VMOBJ_FILE)
            vnode = (struct vnode *) vm_entry->vm_object->p;

        sz += snprintf(maps_buf + sz, sizeof(maps_buf) - sz,
                "%x-%x %s %x %x %x %s\n",
                vm_entry->base,  /* its start */
                vm_entry->base + vm_entry->size,  /* its end */
                perm,   /* its permissions */
                vm_entry->off, /* its offset in the file */
                vnode? vnode->dev : 0, /* the file's device */
                vnode? vnode->ino : 0, /* the file's inode */
                desc); 
    }
    
    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, maps_buf + off, ret);
        return ret;
    }

    return 0;
}

/** Makes a process's `vmstats`: its mappings and their vm objects. */
static ssize_t procfs_proc_vmstats(int pid, off_t off, size_t size, void *buf)
{
    struct proc *proc = proc_pid_find(pid);

    if (!proc)
        return -ESRCH;

    char vmstats_buf[4096];

    int sz = 0;

    struct queue *vm_entries = &proc->vm_space.vm_entries;

    queue_for (node, vm_entries) {
        struct vm_entry *vm_entry = node->value;

        char perm[5] = {0};
        perm[0] = vm_entry->flags & VM_UR? 'r' : '-';
        perm[1] = vm_entry->flags & VM_UW? 'w' : '-';
        perm[2] = vm_entry->flags & VM_UX? 'x' : '-';
        perm[3] = vm_entry->flags & VM_SHARED? 's' : 'p';

        const char *type = "";
        if (vm_entry->vm_object) {
            switch (vm_entry->vm_object->type) {
                case VMOBJ_FILE: type = "file"; break;
                case VMOBJ_ZERO: type = "anon"; break;
            }
        }

        sz += snprintf(vmstats_buf + sz, sizeof(vmstats_buf) - sz,
                "%p-%p %s %p %p %s\n",
                vm_entry->base,
                vm_entry->base + vm_entry->size,
                perm,
                vm_entry->off,
                vm_entry->vm_object,
                type);
    }
    
    if (off < sz) {
        ssize_t ret = MIN(size, (size_t)(sz - off));
        memcpy(buf, vmstats_buf + off, ret);
        return ret;
    }

    return 0;
}

/** A file of a process's directory: its name, and what makes its text. */
struct procfs_proc_entry {
    char *name;
    ssize_t (*read)(int pid, off_t off, size_t size, void *buf);
};

static struct procfs_proc_entry proc_entries[] = {
    {"status", procfs_proc_status},
    {"maps", procfs_proc_maps},
    {"vmstats", procfs_proc_vmstats},
};

#define PROCFS_PROC_ENTRIES  (sizeof(proc_entries)/sizeof(proc_entries[0]))

/* the inode number of a process's directory; its files' follow it. They
 * start from process 0's, which isn't inode 0 */
#define PROC_INO(pid)   (((pid) + 1) * (PROCFS_PROC_ENTRIES + 1))
#define INO_PID(ino)    ((ino) / (PROCFS_PROC_ENTRIES + 1) - 1)

/** Makes the text of a file of procfs, from an offset, into a kernel
 * buffer. Returns how many bytes there are, or a negative errno. */
static ssize_t procfs_entry_read(struct vnode *node, off_t offset, size_t size, char *buf)
{
    size_t id = (size_t) node->p;

    if ((ssize_t) node->ino < 0) {
        if (id < PROCFS_ENTRIES)
            return entries[id].read(offset, size, buf);
    } else {
        size_t pid = INO_PID(node->ino);
        id -= 1;
        if (id < PROCFS_PROC_ENTRIES)
            return proc_entries[id].read(pid, offset, size, buf);
    }

    return -ENOSYS;
}

/** Reads a file of procfs: its text is made a page at a time, and copied
 * out through the iobuf. */
static ssize_t procfs_read(struct vnode *node, off_t offset, struct iobuf *io)
{
    char *buf = kmalloc(PAGE_SIZE, &M_BUFFER, 0);

    if (!buf)
        return -ENOMEM;

    size_t size = io->resid, done = 0;
    ssize_t r = 0;

    while (done < size) {
        size_t len = MIN(size - done, PAGE_SIZE);

        if ((r = procfs_entry_read(node, offset + done, len, buf)) <= 0)
            break;

        size_t made = r;

        if ((r = iobuf_out(io, buf, made)) < 0)
            break;

        done += r;

        /* the end of the text, or a fault */
        if ((size_t) r < len)
            break;
    }

    kfree(buf);
    return done ? (ssize_t) done : r;
}

/** Reads a directory's entry at an offset. The entries are `.` and `..`,
 * then the top directory's files, `self` and the processes, or a
 * process's files. */
static ssize_t procfs_readdir(struct vnode *vnode, off_t offset, struct dirent *dirent)
{
    if (vnode == procfs_root) {
        if (offset == 0) {
            dirent->d_ino = 0;
            strcpy(dirent->d_name, ".");
            return 1;
        } else if (offset == 1) {
            dirent->d_ino = 1;
            strcpy(dirent->d_name, "..");
            return 1;
        }

        offset -= 2;

        if ((size_t) offset < PROCFS_ENTRIES) {
            strcpy(dirent->d_name, entries[offset].name);
            return 1;
        }

        /* then self, and the processes */
        offset -= PROCFS_ENTRIES;
        
        if (!offset) {
            snprintf(dirent->d_name, 10, "self");
            return 1;
        }

        --offset;

        queue_for (node, procs) {
            if (!offset) {
                struct proc *proc = node->value;
                snprintf(dirent->d_name, 10, "%d", proc->pid);
                return 1;
            }

            --offset;
        }
    } else if (vnode->ino > 0) {
        size_t pid = INO_PID(vnode->ino);

        if (offset == 0) {
            dirent->d_ino = 0;
            strcpy(dirent->d_name, ".");
            return 1;
        } else if (offset == 1) {
            dirent->d_ino = 1;
            strcpy(dirent->d_name, "..");
            return 1;
        }

        offset -= 2;

        if ((size_t) offset < PROCFS_PROC_ENTRIES) {
            strcpy(dirent->d_name, proc_entries[offset].name);
            return 1;
        }
    }

    return 0;
}

/** Finds the entry of a directory that has a name, and its inode number. */
static int procfs_finddir(struct vnode *parent, const char *name, struct dirent *dirent)
{
    if (parent->ino == (ino_t) procfs_root) {
        for (size_t i = 0; i < PROCFS_ENTRIES; ++i) {
            if (!strcmp(name, entries[i].name)) {
                dirent->d_ino  = -(i + 1);
                return 0;
            }
        }

        if (!strcmp(name, "self")) {
            dirent->d_ino = PROC_INO(curproc->pid);
            return 0;
        }

        queue_for (node, procs) {
            struct proc *proc = node->value;
            char buf[10];
            snprintf(buf, 10, "%d", proc->pid);

            if (!strcmp(buf, name)) {
                dirent->d_ino = PROC_INO(proc->pid);
                return 0;
            }
        }
    } else if (parent->ino > 0) {
        size_t pid = INO_PID(parent->ino);

        /* a process's directory's parent is /proc */
        if (!strcmp(name, "..")) {
            dirent->d_ino = (vino_t) procfs_root;
            return 0;
        }

        for (size_t i = 0; i < PROCFS_PROC_ENTRIES; ++i) {
            if (!strcmp(proc_entries[i].name, name)) {
                dirent->d_ino  = PROC_INO(pid) + i + 1;
                return 0;
            }
        }
    }

    return -ENOENT;
}

/** Gets the vnode of an inode number, making it if it isn't in use. */
static int procfs_vget(struct vnode *super, ino_t ino, struct vnode **vnode)
{
    int err = 0;

    if (ino == (vino_t) procfs_root) {
        if (vnode) *vnode = procfs_root;
        return 0;
    } else if ((ssize_t) ino < 0) {
        intptr_t id = -ino - 1;

        if ((size_t) id < PROCFS_ENTRIES) {
            struct vnode *node = NULL;
            if (!(node = vcache_find(&procfs_vcache, ino))) { /* not in use yet */
                node = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
                if (!node) goto e_nomem;

                node->ino  = ino;
                node->mode = S_IFREG | 0555;
                node->fs   = &procfs;
                node->p    = (void *) id;

                struct timespec ts;
                gettime(&ts);

                node->ctime = ts;
                node->atime = ts;
                node->mtime = ts;

                vcache_insert(&procfs_vcache, node);
            }

            if (vnode) *vnode = node;
            return 0;
        } else {
            return -ENOENT;
        }
    } else {
        size_t pid = INO_PID(ino);
        size_t id  = ino % (PROCFS_PROC_ENTRIES + 1);
        struct vnode *node = NULL;

        if (!(node = vcache_find(&procfs_vcache, ino))) { /* not in use yet */
            node = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
            if (!node) goto e_nomem;

            node->ino = ino;

            if (id) {
                node->mode = S_IFREG | 0555;
            } else {
                node->mode = S_IFDIR | 0555;
            }

            node->fs   = &procfs;
            node->p    = (void *) id;

            struct timespec ts;
            gettime(&ts);

            node->ctime = ts;
            node->atime = ts;
            node->mtime = ts;

            vcache_insert(&procfs_vcache, node);
        }

        if (vnode) *vnode = node;

        return 0;
    }

e_nomem:
    err = -ENOMEM;
error:
    return err;
}

/** Frees a vnode that has no references left; the root stays. */
static int procfs_close(struct vnode *vnode)
{
    /* the root vnode is never freed */
    if (vnode == procfs_root) {
        return 0;
    }

    if (vnode->ref == 0) {
        if (vcache_find(&procfs_vcache, vnode->ino))
            vcache_remove(&procfs_vcache, vnode);

        vnode_pages_free(vnode);
        kfree(vnode);
    }

    return 0;
}

/** Sets procfs up at boot: its root directory, and its vnode cache. */
static int procfs_init()
{
    procfs_root = kmalloc(sizeof(struct vnode), &M_VNODE, M_ZERO);
    if (!procfs_root) return -ENOMEM;

    procfs_root->mode = S_IFDIR | 0555;
    procfs_root->ino  = (vino_t) procfs_root;
    procfs_root->fs   = &procfs;
    procfs_root->ref  = 1;

    struct timespec ts;
    gettime(&ts);

    procfs_root->ctime = ts;
    procfs_root->atime = ts;
    procfs_root->mtime = ts;

    vcache_init(&procfs_vcache);

    vfs_install(&procfs);
    return 0;
}

/** Mounts procfs on a directory: its one root directory is bound there.
 * Returns 0, or `-EBUSY` if something is mounted on the directory, or
 * procfs is mounted elsewhere already. */
static int procfs_mount(struct vnode *dir, int flags, void *data)
{
    return vfs_bind(dir, procfs_root);
}

struct fs procfs = {
    .name  = "procfs",
    .nodev = 1,
    .init  = procfs_init,
    .mount = procfs_mount,

    .vops = {
        .read    = procfs_read,
        .readdir = procfs_readdir,
        .finddir = procfs_finddir,
        .vget    = procfs_vget,
        .close   = procfs_close,
    },

    .fops = {
        .open    = posix_file_open,
        .read    = posix_file_read,
        .readdir = posix_file_readdir,
        .close   = posix_file_close,

        .eof     = __vfs_eof_always,    /* a read is a snapshot: its end is the end */
    },
};

MODULE_INIT(procfs, procfs_init, NULL)
