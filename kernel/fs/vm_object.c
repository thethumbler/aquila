/** \file
 * The page cache: a file's pages in memory. A vnode's vm object keeps the
 * file's pages by their offset. Its pager reads a page in the first time
 * it is needed: when a process that maps the file touches it (see
 * `mm/fault.c`), or when a regular file of a filesystem on a disk is read
 * (`vm_cache_read()`). Every process that maps or reads the file shares
 * the same pages.
 *
 * The pages stay as long as there is memory for them. Every cached page
 * is on one list, the page used least recently first; when no page of
 * memory is left, `mm_page_alloc()` takes back the oldest ones
 * (`vm_cache_reclaim()`). Only the pages of files that nothing maps are
 * taken back: a mapped page may be in a process's page tables, and
 * nothing says which.
 *
 * Writes go to the filesystem, which writes them through the disks'
 * block cache, and the cached pages are read again from it
 * (`vnode_pages_update()`).
 */

#include <core/system.h>
#include <core/string.h>
#include <fs/vfs.h>
#include <fs/iobuf.h>
#include <mm/vm.h>
#include <mm/pmap.h>

static struct vm_pager vnode_pager;

/* the cached pages of every file, the one used least recently first */
static struct list lru;

/* how many pages a read that goes on from the last one reads in at once:
 * 64 KiB, as much as the disks' block cache reads in one command */
#define READ_AHEAD  16

/* how much is copied out of a page at a time, through the stack (see
 * vm_cache_read()) */
#define CHUNK       512

/** Says whether a cached page is the one at the offset `_b` points to. */
static int vm_page_eq(void *_a, void *_b)
{
    struct vm_page *a = (struct vm_page *) _a;
    size_t *b = (size_t *) _b;

    return a->off == *b;
}

/**
 * Gets a vnode's vm object, making it the first time.
 *
 * @param vnode  the vnode
 * @return its vm object, or NULL if `vnode` is NULL or there was no
 *         memory
 */
struct vm_object *vm_object_vnode(struct vnode *vnode)
{
    if (!vnode)
        return NULL;

    if (!vnode->vm_object) {
        struct vm_object *vm_object;

        vm_object = kmalloc(sizeof(struct vm_object), &M_VM_OBJECT, M_ZERO);
        if (!vm_object) return NULL;

        vm_object->type  = VMOBJ_FILE;
        vm_object->pages = hashmap_new(0, vm_page_eq);

        if (!vm_object->pages) {
            kfree(vm_object);
            return NULL;
        }

        vm_object->pager = &vnode_pager;
        vm_object->p = vnode;

        vnode->vm_object = vm_object;
    }

    return vnode->vm_object;
}

/** Returns a file's cached page at `off` (on a page), or NULL. */
static struct vm_page *page_lookup(struct vm_object *vm_object, size_t off)
{
    hash_t hash = hashmap_digest(&off, sizeof(off));
    struct hashmap_node *node = hashmap_lookup(vm_object->pages, hash, &off);

    return node ? (struct vm_page *) node->entry : NULL;
}

/** Reads `size` bytes of a file at `off` into a kernel buffer, from the
 * filesystem itself. */
static ssize_t file_read(struct vnode *vnode, size_t off, size_t size, void *buf)
{
    struct iobuf io;
    iobuf_init(&io, buf, size, 0);
    return vfs_read_raw(vnode, off, &io);
}

/**
 * Reads pages of a file into its vm object, from `off` on: `n` of them,
 * or fewer, up to the first page that is cached already or past the end
 * of the file (the page at `off` itself is always read). They are read
 * with one read of the filesystem. Past the end of the file, a page reads
 * as zeros.
 *
 * @return 0, the page at `off` in `*page`; `-ENOMEM` if there was no
 *         memory for it (or for the read), or `-EIO` if the read failed
 */
static int pages_in(struct vm_object *vm_object, size_t off, size_t n, struct vm_page **page)
{
    struct vnode *vnode = (struct vnode *) vm_object->p;

    off = PAGE_ALIGN(off);

    /* not past the end of the file, nor over a page that is there */
    size_t end = PAGE_ROUND(vnode->size);
    size_t i;

    for (i = 1; i < n; ++i) {
        size_t o = off + i * PAGE_SIZE;

        if (o >= end || page_lookup(vm_object, o))
            break;
    }

    n = i;

    /* read into a buffer of its own, since reading may sleep (on the
     * disk); a short read leaves zeros, not stale memory */
    char *buf = kmalloc(n * PAGE_SIZE, &M_BUFFER, M_ZERO);

    if (!buf && n > 1)
        buf = kmalloc((n = 1) * PAGE_SIZE, &M_BUFFER, M_ZERO);

    if (!buf)
        return -ENOMEM;

    /* a page that can't be read isn't zeros: the read or the fault fails,
     * with the filesystem's error (-ENOMEM too, deeper down) */
    ssize_t r = file_read(vnode, off, n * PAGE_SIZE, buf);

    if (r < 0) {
        kfree(buf);
        return r == -ENOMEM ? -ENOMEM : -EIO;
    }

    for (i = 0; i < n; ++i) {
        size_t o = off + i * PAGE_SIZE;

        /* another thread may have read it in meanwhile */
        if (page_lookup(vm_object, o))
            continue;

        struct vm_page *vm_page = mm_page_alloc();

        if (!vm_page)
            break;

        vm_page->vm_object = vm_object;
        vm_page->off = o;
        vm_page->ref = 1;

        pmap_page_write(vm_page->paddr, 0, PAGE_SIZE, buf + i * PAGE_SIZE);

        /* no memory to keep it in the object: it isn't cached */
        if (vm_object_page_insert(vm_object, vm_page)) {
            mm_page_dealloc(vm_page->paddr);
            break;
        }

        list_append(&lru, &vm_page->lru);
    }

    kfree(buf);

    vm_object->ra_next = off + n * PAGE_SIZE;

    /* the first page is missing only if there was no memory for it */
    *page = page_lookup(vm_object, off);
    return *page ? 0 : -ENOMEM;
}

/** How many pages to read in for a page that isn't cached: many if the
 * file is read from its start, or from where the last read in ended,
 * one otherwise. */
static size_t read_ahead(struct vm_object *vm_object, size_t off)
{
    return off == 0 || off == vm_object->ra_next ? READ_AHEAD : 1;
}

/**
 * Reads a page of a file into its vm object: the vnode pager's page in,
 * for a page fault. Pages after it are read too, if the file is read in
 * order. Past the end of the file, the page reads as zeros.
 *
 * @param vm_object  the file's vm object
 * @param off        the page's offset in the file
 * @param page       set to the page
 * @return 0, `-ENOMEM` if there was no memory, or `-EIO` if the read
 *         failed
 */
int vnode_page_in(struct vm_object *vm_object, size_t off, struct vm_page **page)
{
    off = PAGE_ALIGN(off);

    if ((*page = page_lookup(vm_object, off)))
        return 0;

    return pages_in(vm_object, off, read_ahead(vm_object, off), page);
}

/**
 * Reads a regular file through the page cache, for `read()`: from its
 * cached pages, reading in the ones that aren't there first.
 *
 * The data is copied out a piece at a time through the stack, and each
 * piece's page looked up again: copying out may sleep (a page of the
 * reader's memory, brought in from the disk), and meanwhile the page may
 * be taken back for its memory.
 *
 * @param vnode  the file
 * @param off    where to read from
 * @param io     where the data goes, and how much is wanted
 * @return the number of bytes read, 0 at the end of the file, or a
 *         negative errno (`-ENOMEM` or `-EIO` if a page couldn't be read
 *         in)
 */
ssize_t vm_cache_read(struct vnode *vnode, off_t off, struct iobuf *io)
{
    if (off < 0)
        return -EINVAL;

    struct vm_object *vm_object = vm_object_vnode(vnode);

    /* no memory for the vm object: read the file itself */
    if (!vm_object)
        return vfs_read_raw(vnode, off, io);

    char chunk[CHUNK];
    size_t pos = off, done = 0;

    while (io->resid && pos < vnode->size) {
        size_t page = PAGE_ALIGN(pos), in = pos - page;
        size_t len = MIN(MIN(io->resid, PAGE_SIZE - in), vnode->size - pos);

        len = MIN(len, CHUNK);

        struct vm_page *vm_page = page_lookup(vm_object, page);
        int err;

        if (!vm_page && (err = pages_in(vm_object, page, read_ahead(vm_object, page), &vm_page)))
            return done ? (ssize_t) done : err;

        /* used now: it goes to the end of the list */
        list_remove(&vm_page->lru);
        list_append(&lru, &vm_page->lru);

        pmap_page_read(vm_page->paddr, in, len, chunk);

        ssize_t r = iobuf_out(io, chunk, len);

        if (r < 0)
            return done ? (ssize_t) done : r;

        done += r;
        pos  += r;

        /* a fault stopped the copy */
        if ((size_t) r < len)
            break;
    }

    return done;
}

/**
 * Takes back memory from the page cache: frees pages, the ones used
 * least recently first, of files that nothing maps. `mm_page_alloc()`
 * calls it when there is no page of memory left.
 *
 * @param want  how many pages to free
 * @return how many were freed
 */
size_t vm_cache_reclaim(size_t want)
{
    size_t freed = 0;

    list_for (node, &lru) {
        if (freed == want)
            break;

        struct vm_page *vm_page = list_entry(node, struct vm_page, lru);
        struct vm_object *vm_object = vm_page->vm_object;

        /* mapped: the page may be in a process's page tables */
        if (vm_object->ref)
            continue;

        size_t off = vm_page->off;
        hash_t hash = hashmap_digest(&off, sizeof(off));
        struct hashmap_node *hnode = hashmap_lookup(vm_object->pages, hash, &off);

        if (hnode)
            hashmap_node_remove(vm_object->pages, hnode);

        list_remove(&vm_page->lru);
        mm_page_dealloc(vm_page->paddr);
        freed++;
    }

    return freed;
}

/**
 * Says how many pages the page cache has, for `/proc/meminfo`.
 *
 * @return the number of cached pages of files
 */
size_t vm_cache_pages(void)
{
    return lru.count;
}

/**
 * Reads a file's cached pages again after a write or a truncation changed
 * a range of it. A page stays where it is, mapped by every process that
 * maps the file, so its contents change but not the page; past the
 * file's end it reads zeros. A private mapping's copies, made when it
 * wrote to a page, are its own and don't change.
 *
 * @param vnode  the file
 * @param start  the start of the range that changed
 * @param end    its end (exclusive)
 */
void vnode_pages_update(struct vnode *vnode, off_t start, off_t end)
{
    struct vm_object *vm_object = vnode->vm_object;

    if (!vm_object || !vm_object->pages->count || end <= start)
        return;

    char *buf = NULL;

    /* each page is looked up after it is read, rather than the map walked:
     * the read may sleep, and meanwhile the page may be read in, or taken
     * back for its memory */
    for (size_t off = PAGE_ALIGN((size_t) start); off < (size_t) end; off += PAGE_SIZE) {
        if (!page_lookup(vm_object, off))
            continue;

        if (!buf && !(buf = kmalloc(PAGE_SIZE, &M_BUFFER, 0))) {
            printk("vm: no memory to update a file's cached pages\n");
            return;
        }

        memset(buf, 0, PAGE_SIZE);

        if (file_read(vnode, off, PAGE_SIZE, buf) < 0)
            continue;

        struct vm_page *vm_page = page_lookup(vm_object, off);

        if (vm_page)
            pmap_page_write(vm_page->paddr, 0, PAGE_SIZE, buf);
    }

    kfree(buf);
}

/**
 * Frees a vnode's vm object and its cached pages, when the vnode itself
 * is freed. Nothing maps them then, since a mapping holds the vnode
 * (`vm_object_incref()`).
 *
 * @param vnode  the vnode
 */
void vnode_pages_free(struct vnode *vnode)
{
    struct vm_object *vm_object = vnode->vm_object;

    if (!vm_object)
        return;

    hashmap_for (qnode, vm_object->pages) {
        struct hashmap_node *node = (struct hashmap_node *) qnode->value;
        struct vm_page *vm_page = (struct vm_page *) node->entry;

        list_remove(&vm_page->lru);
        mm_page_dealloc(vm_page->paddr);
    }

    hashmap_free(vm_object->pages);
    kfree(vm_object);
    vnode->vm_object = NULL;
}

static struct vm_pager vnode_pager = {
    .in = vnode_page_in,
    .out = NULL,
};
