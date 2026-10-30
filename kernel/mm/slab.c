/** \file
 * Slabs: the kernel's small objects.
 *
 * `kmalloc()` takes objects of up to `SLAB_MAX` bytes from here, and the
 * bigger ones from kvmem (`mm/kvmem.c`). A slab is one page of objects of
 * one size (a power of two, from 16 bytes) and one malloc type, with its
 * header at the start of the page. Each malloc type has, for each size, a
 * list of its slabs that have free objects, and each slab keeps a list of
 * its free objects. So allocating and freeing take the same time, however
 * many objects there are.
 *
 * The slabs' pages are mapped in a region of their own in the kernel's
 * address space (`ARCH_SLAB_BASE`), which tells `kfree()` that an object
 * is a slab's, and a bitmap says which pages of the region are in use. A
 * slab whose objects are all free gives its page back to the buddy
 * allocator.
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/string.h>

#include <ds/queue.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/pmap.h>
#include <mm/vm.h>

/** A slab's header, at the start of its page. */
struct slab {
    struct slab *next, *prev;   /* the type's slabs of this size that have free objects */
    struct malloc_type *type;
    void *free;                 /* its free objects, linked through their first word */
    uint32_t size;              /* the size of its objects */
    uint32_t used;              /* how many of them are in use */
};

/* the objects start after the header, aligned as kmalloc()'s are */
#define SLAB_HEADER     ((sizeof(struct slab) + 15) & ~(size_t) 15)
#define SLAB_PAGES      (ARCH_SLAB_SIZE / PAGE_SIZE)

static uint8_t slab_map[SLAB_PAGES / 8];
static size_t slab_first_free;

size_t slab_used;   /* bytes of the slabs' pages, for /proc/meminfo */

/** Returns the size class for objects of `size` bytes: the smallest power
 * of two from `SLAB_MIN` that holds them, as an index (0 for `SLAB_MIN`). */
static int slab_class(size_t size)
{
    int class = 0;

    while (((size_t) SLAB_MIN << class) < size)
        ++class;

    return class;
}

/** Maps a page of memory at a free page of the slab region, for a new
 * slab. Returns NULL if the region is full or there is no memory, and
 * then `kmalloc()` returns NULL. */
static struct slab *slab_page_alloc(void)
{
    /* the page first: getting one may free objects (the page cache gives
     * memory back, see mm_page_alloc()), and slabs with them, which
     * changes which pages of the region are free */
    struct vm_page *vm_page = mm_page_alloc();

    if (!vm_page)
        return NULL;

    size_t i = slab_first_free;

    while (i < SLAB_PAGES && (slab_map[i / 8] & (1 << (i % 8))))
        ++i;

    if (i >= SLAB_PAGES) {
        mm_page_dealloc(vm_page->paddr);
        return NULL;
    }

    vaddr_t vaddr = ARCH_SLAB_BASE + i * PAGE_SIZE;

    if (pmap_add(kvm_space.pmap, vaddr, vm_page->paddr, VM_KRW)) {
        mm_page_dealloc(vm_page->paddr);
        return NULL;
    }

    slab_map[i / 8] |= 1 << (i % 8);
    slab_first_free = i + 1;
    slab_used += PAGE_SIZE;

    return (struct slab *) vaddr;
}

/** Unmaps a slab's page, and gives it back to the buddy allocator. */
static void slab_page_free(struct slab *slab)
{
    vaddr_t vaddr = (vaddr_t) slab;
    size_t i = (vaddr - ARCH_SLAB_BASE) / PAGE_SIZE;
    paddr_t paddr = arch_page_get_mapping(kvm_space.pmap, vaddr);

    pmap_remove(kvm_space.pmap, vaddr, vaddr + PAGE_SIZE);
    mm_page_dealloc(paddr);
    slab_used -= PAGE_SIZE;

    slab_map[i / 8] &= ~(1 << (i % 8));

    if (i < slab_first_free)
        slab_first_free = i;
}

/** Takes `slab` off a list of slabs with free objects. */
static void slab_unlink(struct slab **list, struct slab *slab)
{
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        *list = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->next = slab->prev = NULL;
}

/** Puts `slab` at the head of a list of slabs with free objects. */
static void slab_link(struct slab **list, struct slab *slab)
{
    slab->prev = NULL;
    slab->next = *list;

    if (*list)
        (*list)->prev = slab;

    *list = slab;
}

/**
 * Allocates a small object, from a slab of its malloc type and size
 * class, making a new slab if none has a free object. `kmalloc()` calls
 * it for objects of up to `SLAB_MAX` bytes.
 *
 * @param size  the size wanted, in bytes, at most `SLAB_MAX`
 * @param type  the malloc type it's counted as
 * @return the object, or NULL if there was no memory for a new slab
 */
void *slab_alloc(size_t size, struct malloc_type *type)
{
    int class = slab_class(size);
    struct slab **list = &type->slabs[class];
    struct slab *slab = *list;

    if (!slab) {
        if (!(slab = slab_page_alloc()))
            return NULL;

        slab->type = type;
        slab->size = SLAB_MIN << class;
        slab->used = 0;
        slab->free = NULL;

        /* every object free, the first one at the head of the list */
        for (size_t off = PAGE_SIZE - slab->size; off >= SLAB_HEADER; off -= slab->size) {
            void **obj = (void **) ((char *) slab + off);
            *obj = slab->free;
            slab->free = obj;
        }

        slab_link(list, slab);
    }

    void **obj = slab->free;
    slab->free = *obj;
    slab->used++;

    /* the slab is full now: it leaves the list */
    if (!slab->free)
        slab_unlink(list, slab);

    type->nr++;
    type->total += slab->size;

    return obj;
}

/**
 * Says whether an object is a slab's, for `kfree()`.
 *
 * @param ptr  the object
 * @return whether it is in the slab region
 */
int slab_owns(void *ptr)
{
    uintptr_t addr = (uintptr_t) ptr;
    return addr >= ARCH_SLAB_BASE && addr < ARCH_SLAB_BASE + ARCH_SLAB_SIZE;
}

/**
 * Frees an object of a slab. The slab goes back on its list if it was
 * full, and gives its page back if it is empty now.
 *
 * @param ptr  the object, which `slab_alloc()` gave
 */
void slab_free(void *ptr)
{
    struct slab *slab = (struct slab *) PAGE_ALIGN((uintptr_t) ptr);
    struct malloc_type *type = slab->type;
    struct slab **list = &type->slabs[slab_class(slab->size)];

    int was_full = !slab->free;

    *(void **) ptr = slab->free;
    slab->free = ptr;
    slab->used--;

    type->nr--;
    type->total -= slab->size;

    if (!slab->used) {
        if (!was_full)
            slab_unlink(list, slab);

        slab_page_free(slab);
    } else if (was_full) {
        slab_link(list, slab);
    }
}
