#ifndef _MM_VM_H
#define _MM_VM_H

#include <core/system.h>
#include <core/printk.h>
#include <ds/list.h>

struct vm_space;
struct vm_entry;
struct vm_object;
struct vm_page;

MALLOC_DECLARE(M_VM_ENTRY);
MALLOC_DECLARE(M_VM_OBJECT);
MALLOC_DECLARE(M_VM_ANON);
MALLOC_DECLARE(M_VM_AREF);

#include <ds/queue.h>

/*
 * Virtual memory: address spaces, their mappings, and where the mappings'
 * pages come from (anons and vm objects). See docs/kernel/memory.md.
 */

/** An address space: its mappings, and its page tables. */
struct vm_space {
    /** its page tables (the architecture's) */
    struct pmap  *pmap;

    /** its mappings (map entries), in address order */
    struct queue  vm_entries;
};

#include <fs/vfs.h>
#include <mm/mm.h>
#include <ds/hashmap.h>

/* A mapping's flags: its permissions, and more. */
#define VM_KR         0x0001         /**< kernel read */
#define VM_KW         0x0002         /**< kernel write */
#define VM_KX         0x0004         /**< kernel execute */
#define VM_UR         0x0008         /**< user read */
#define VM_UW         0x0010         /**< user write */
#define VM_UX         0x0020         /**< user execute */
#define VM_PERM       0x003F         /**< permissions mask */
#define VM_NOCACHE    0x0040         /**< not cached (device memory) */
#define VM_SHARED     0x0080         /**< shared: a fork doesn't copy it */
#define VM_COPY       0x0100         /**< to be copied before it's written to */
#define VM_MAYWRITE   0x0200         /**< a file's shared mapping that may be
                                          made writable (the file is open
                                          for writing) */

#define VM_KRW  (VM_KR|VM_KW)        /**< kernel read/write */
#define VM_KRX  (VM_KR|VM_KX)        /**< kernel read/execute */
#define VM_KWX  (VM_KW|VM_KX)        /**< kernel write/execute */
#define VM_KRWX (VM_KR|VM_KW|VM_KX)  /**< kernel read/write/execute */
#define VM_URW  (VM_UR|VM_UW)        /**< user read/write */
#define VM_URX  (VM_UR|VM_UX)        /**< user read/execute */
#define VM_UWX  (VM_UW|VM_UX)        /**< user write/execute */
#define VM_URWX (VM_UR|VM_UW|VM_UX)  /**< user read/write/execute */

/* The kinds of vm objects. */
#define VMOBJ_ZERO    0x0000         /**< pages of zeros */
#define VMOBJ_FILE    0x0001         /**< a file's pages */

/**
 * A mapping (map entry): a range of an address space, its permissions,
 * and where its pages come from.
 */
struct vm_entry {
    /** physical memory to map all at once (vm_map()), or 0 for pages that
     *  come in on page faults */
    paddr_t  paddr;

    /** where the mapping starts in its address space */
    vaddr_t base;

    /** its size */
    size_t size;

    /** its permissions and flags (VM_*) */
    uint32_t flags;

    /** its anon: the pages the process wrote to */
    struct vm_anon *vm_anon;

    /** its object: what its other pages come from (a file) */
    struct vm_object *vm_object;

    /** where the mapping starts in its object */
    size_t off;

    /** its node in its address space's list of mappings */
    struct qnode *qnode;

    /** its address space, whose page tables map its pages */
    struct vm_space *vm_space;

    /** its place on its object's list of mappings (`vm_object_map()`) */
    struct list_node obj_link;
};

/** A vm object's pager: reads its pages in, and writes them out. */
struct vm_pager {
    /** reads in the page at `off` into `*page`: 0, `-ENOMEM` if there was
     *  no memory, or `-EIO` if it couldn't be read */
    int (*in)(struct vm_object *vm_object, size_t off, struct vm_page **page);

    /** writes out the page at `off` */
    int (*out)(struct vm_object *vm_object, size_t off);
};

/**
 * An anon's reference to a page (an aref). Anons that a fork shares share
 * their arefs, and `ref` counts them, so the page is copied before one of
 * them writes to it.
 */
struct vm_aref {
    /** the page */
    struct vm_page *vm_page;

    /** how many anons have the aref */
    size_t ref;

    /** its flags (VM_COPY) */
    uint32_t flags;
};

/** An anon: a mapping's private pages, those the process wrote to. */
struct vm_anon {
    /** its arefs, by the offset of their page in the mapping */
    struct hashmap *arefs;

    /** how many mappings share the anon */
    size_t ref;

    /** its flags (VM_COPY: to be copied before it's written to) */
    int flags;
};

/** A vm object: what a mapping's pages come from (a file), and its pages
 *  that have been read in. */
struct vm_object {
    /** its pages that have been read in, by their offset */
    struct hashmap *pages;

    /** its kind (VMOBJ_*) */
    int type;

    /** how many mappings it has */
    size_t ref;

    /** its pager */
    struct vm_pager *pager;

    /** the pager's own data (a file's vnode) */
    void *p;

    /** where a sequential read would read in next, for read-ahead */
    size_t ra_next;

    /** the map entries that map it, in any address space: how its pages
     *  are found in page tables (a reverse map, Linux's i_mmap) */
    struct list mappings;

    /** how many of its pages are dirty (written to through a shared
     *  mapping, not written back yet), and since when, in
     *  `arch_rtime_ns()` time */
    size_t ndirty;
    uint64_t dirty_since;

    /** its place on the page cache's list of objects with dirty pages */
    struct list_node dirty_link;
};

/** A page of physical memory (pages[] has one for every page of RAM). */
struct vm_page {
    paddr_t  paddr; /**< its physical address */

    struct vm_object *vm_object; /**< the object it belongs to, or NULL */

    size_t off; /**< its offset in its object or anon */

    size_t ref; /**< how many references it has */

    /** a file's page: its place on the page cache's list, the page used
     *  least recently first */
    struct list_node lru;

    /** a file's page: how many are using it (a fault mapping it), which
     *  the page cache doesn't take back meanwhile */
    unsigned short pin;

    /** a file's page: written to through a shared mapping, and not written
     *  back to the file yet */
    unsigned char dirty;
};

/* The kernel's address space. */
extern struct vm_space kvm_space;

/* mm/kvmem.c: sets up kmalloc(); its self-checks */
void kvmem_setup(void);
void kvmem_selftest(void);
void kvmem_guardtest(void);

/* mm/vmm.c: maps and unmaps a map entry's range */
int  vm_map(struct vm_space *vm_space, struct vm_entry *vm_entry);
void vm_unmap_full(struct vm_space *vm_space, struct vm_entry *vm_entry);

/* mm/vm_space.c: address spaces */
int  vm_space_fork(struct vm_space *parent, struct vm_space *child);
void vm_space_destroy(struct vm_space *vm_space);
struct vm_entry *vm_space_find(struct vm_space *vm_space, vaddr_t vaddr);
int  vm_space_insert(struct vm_space *vm_space, struct vm_entry *vm_entry);
int  vm_space_split(struct vm_space *vm_space, struct vm_entry *vm_entry, vaddr_t addr);
int  vm_space_protect(struct vm_space *vm_space, vaddr_t start, vaddr_t end, uint32_t perm);
int  vm_space_remove(struct vm_space *vm_space, vaddr_t start, vaddr_t end);
int  vm_space_msync(struct vm_space *vm_space, vaddr_t start, vaddr_t end);

/* mm/vm_entry.c: map entries */
struct vm_entry *vm_entry_new(void);
void vm_entry_destroy(struct vm_entry *vm_entry);

/* mm/vm_anon.c: anons */
struct vm_anon *vm_anon_new(void);
struct vm_anon *vm_anon_copy(struct vm_anon *vm_anon);
struct vm_anon *vm_anon_split(struct vm_anon *vm_anon, size_t off);
void vm_anon_incref(struct vm_anon *vm_anon);
void vm_anon_decref(struct vm_anon *vm_anon);
void vm_anon_destroy(struct vm_anon *vm_anon);

/* fs/vm_object.c and mm/vm_object.c: vm objects, and a file's pages (the
 * page cache) */
struct vm_object *vm_object_vnode(struct vnode *vnode);
void vnode_pages_update(struct vnode *vnode, off_t start, off_t end);
void vnode_pages_free(struct vnode *vnode);
ssize_t vm_cache_read(struct vnode *vnode, off_t off, struct iobuf *io);
size_t vm_cache_reclaim(size_t want);
size_t vm_cache_pages(void);
size_t vm_cache_dirty_pages(void);
void vm_cache_page_dirty(struct vm_object *vm_object, struct vm_page *vm_page);
void vm_cache_dirty_throttle(void);
int  vm_cache_flush(struct vnode *vnode);
int  vm_cache_flush_all(void);
void vm_cache_flusher_start(void);
int  vm_object_page_insert(struct vm_object *vm_object, struct vm_page *vm_page);
void vm_object_incref(struct vm_object *vm_object);
void vm_object_decref(struct vm_object *vm_object);
void vm_object_map(struct vm_object *vm_object, struct vm_entry *vm_entry);
void vm_object_unmap(struct vm_object *vm_object, struct vm_entry *vm_entry);
void vm_object_page_unmap(struct vm_object *vm_object, struct vm_page *vm_page);
void vm_object_page_protect(struct vm_object *vm_object, struct vm_page *vm_page);

#endif  /* ! _MM_VM_H */
