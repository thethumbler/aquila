/** \file
 * `kmalloc()` and `kfree()`: the kernel's memory for its own structures.
 *
 * Objects of up to `SLAB_MAX` bytes, most of them, come from slabs
 * (`mm/slab.c`). Bigger objects get whole pages of their own, in the
 * kvmem region of the kernel's address space (`ARCH_KVMEM_BASE`): a vmem
 * arena (`mm/vmem.h`) gives the range of addresses, pages of memory are
 * mapped there, and the arena's segment keeps the object's size and
 * malloc type for `kfree()`.
 *
 * Each object's range ends in a guard page, which is never mapped. A
 * write past an object's end faults, and so does a write below its start
 * (the page there is another object's guard, or isn't allocated). So a
 * kernel stack that overflows, or a buffer overrun, ends in a page fault
 * instead of corrupting another object. It costs addresses, not memory.
 *
 * Every allocation is counted by its malloc type (`/proc/kvmem`).
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/string.h>

#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/pmap.h>
#include <mm/vm.h>
#include <mm/vmem.h>

struct queue *malloc_types = QUEUE_NEW();

MALLOC_DEFINE(M_BUFFER, "buffer", "generic buffer");
MALLOC_DEFINE(M_RINGBUF, "ring-buffer", "ringbuffer structure");
MALLOC_DEFINE(M_QUEUE, "queue", "queue structure");
MALLOC_DEFINE(M_QNODE, "queue-node", "queue node structure");
MALLOC_DEFINE(M_HASHMAP, "hashmap", "hashmap structure");
MALLOC_DEFINE(M_HASHMAP_NODE, "hashmap-node", "hashmap node structure");

/* the guard page after each object */
#define KVMEM_GUARD     PAGE_SIZE

static struct vmem kvmem;

size_t kvmem_used;      /* bytes of the objects' pages, for /proc/meminfo */
size_t kvmem_obj_cnt;   /* how many objects there are */

void *slab_alloc(size_t size, struct malloc_type *type);
int  slab_owns(void *ptr);
void slab_free(void *ptr);

/**
 * Sets up the kernel's allocator at boot, once physical memory and the
 * page tables are ready.
 */
void kvmem_setup(void)
{
    /* enqueue() allocates the list's node, so M_QNODE can't be on the
     * list before that node exists: it gets a placeholder first */
    M_QNODE.qnode = (void *) 0xDEADBEEF;
    M_QNODE.qnode = enqueue(malloc_types, &M_QNODE);

    if (vmem_init(&kvmem, "kvmem", ARCH_KVMEM_BASE, ARCH_KVMEM_SIZE, PAGE_SIZE))
        panic("kvmem: no memory for its arena");
}

/** Unmaps the pages of an object at `base`, and gives them back to the
 * buddy allocator. Pages that aren't mapped are skipped. */
static void kvmem_unmap(vaddr_t base, size_t size)
{
    for (vaddr_t va = base; va < base + size; va += PAGE_SIZE) {
        paddr_t paddr = arch_page_get_mapping(kvm_space.pmap, va);

        if (paddr) {
            pmap_remove(kvm_space.pmap, va, va + PAGE_SIZE);
            mm_page_dealloc(paddr);
        }
    }
}

/** Allocates an object of whole pages, with its guard page after it.
 * Returns NULL if there is no memory, or no room left in the region. */
static void *kvmem_alloc(size_t size, struct malloc_type *type)
{
    /* so big that rounding it up (with the guard page) would wrap around
     * to a small size: there's never room for it */
    if (size > (size_t) -1 - PAGE_SIZE - KVMEM_GUARD)
        return NULL;

    size = PAGE_ROUND(size);

    struct vmem_seg *seg = vmem_alloc(&kvmem, size + KVMEM_GUARD);

    if (!seg)
        return NULL;

    for (vaddr_t va = seg->base; va < seg->base + size; va += PAGE_SIZE) {
        struct vm_page *vm_page = mm_page_alloc();

        if (!vm_page || pmap_add(kvm_space.pmap, va, vm_page->paddr, VM_KRW)) {
            if (vm_page)
                mm_page_dealloc(vm_page->paddr);

            kvmem_unmap(seg->base, va - seg->base);
            vmem_free(&kvmem, seg);
            return NULL;
        }
    }

    seg->type  = type;
    seg->bytes = size;

    type->nr++;
    type->total += size;
    kvmem_used += size;
    kvmem_obj_cnt++;

    return (void *) seg->base;
}

/**
 * Allocates memory for one of the kernel's structures.
 *
 * @param size   the size wanted, in bytes
 * @param type   the malloc type it's counted as (`MALLOC_DEFINE()`)
 * @param flags  `M_ZERO` to have it zeroed, or 0
 * @return the object, or NULL if there was no memory for it
 */
void *kmalloc(size_t size, struct malloc_type *type, int flags)
{
    /* the small objects, most of them, from slabs */
    void *obj = size <= SLAB_MAX ? slab_alloc(size, type) : kvmem_alloc(size, type);

    if (!obj)
        return NULL;

    if (type->qnode == NULL)
        type->qnode = enqueue(malloc_types, type);

    if (flags & M_ZERO)
        memset(obj, 0, size);

    return obj;
}

/**
 * Frees an object that `kmalloc()` gave. Freeing anything else (a
 * pointer into an object, or one freed already) is a bug, and panics.
 *
 * @param ptr  the object, or NULL to do nothing
 */
void kfree(void *ptr)
{
    if (!ptr)
        return;

    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    struct vmem_seg *seg = vmem_find(&kvmem, (uintptr_t) ptr);

    /* not the start of an object: a pointer into one, an object freed
     * twice, or something kmalloc() never gave */
    if (!seg) {
        printk("kfree: %p isn't an object kmalloc() gave\n", ptr);
        panic("kfree of something kmalloc() didn't give");
    }

    struct malloc_type *type = seg->type;

    type->nr--;
    type->total -= seg->bytes;
    kvmem_used -= seg->bytes;
    kvmem_obj_cnt--;

    kvmem_unmap(seg->base, seg->bytes);
    vmem_free(&kvmem, seg);
}

/*
 * Self-checks, run when the kernel's command line asks for them (the
 * kvmem.test and kvmem.guard arguments, which tests/kvmem uses).
 */

MALLOC_DEFINE(M_KVMEM_TEST, "kvmem-test", "kvmem self-test object");

/** Returns how many segments, used and free, the arena has. */
static size_t kvmem_segs(void)
{
    size_t n = 0;

    for (struct vmem_seg *seg = kvmem.segs; seg; seg = seg->next)
        ++n;

    return n;
}

#define TEST_OBJS   48

/**
 * Allocates objects of many sizes and checks that each is on pages of its
 * own, that its guard page isn't mapped, and that none overwrites
 * another. Then frees them out of order, and checks that the arena is as
 * it was. Returns NULL if all is well, or what went wrong.
 */
static const char *kvmem_selftest_run(void)
{
    static uint8_t *obj[TEST_OBJS];
    static size_t size[TEST_OBJS];
    size_t inuse = kvmem.inuse, segs = kvmem_segs(), objs = kvmem_obj_cnt;

    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < TEST_OBJS; ++i) {
            if (obj[i])
                continue;

            size[i] = SLAB_MAX + 1 + (i * 1237 + round * 4093) % (6 * PAGE_SIZE);

            if (!(obj[i] = kmalloc(size[i], &M_KVMEM_TEST, 0)))
                return "an allocation failed";

            if ((uintptr_t) obj[i] % PAGE_SIZE)
                return "an object isn't on a page";

            if (arch_page_get_mapping(kvm_space.pmap, (vaddr_t) obj[i] + PAGE_ROUND(size[i])))
                return "a guard page is mapped";

            memset(obj[i], i + 1, size[i]);
        }

        for (int i = 0; i < TEST_OBJS; ++i)
            for (size_t b = 0; obj[i] && b < size[i]; b += 97)
                if (obj[i][b] != (uint8_t) (i + 1))
                    return "an object was overwritten";

        /* every third one, then every other: holes to fill and merge */
        for (int i = round; i < TEST_OBJS; i += 3 - round) {
            kfree(obj[i]);
            obj[i] = NULL;
        }
    }

    if (M_KVMEM_TEST.nr != (size_t) (kvmem_obj_cnt - objs))
        return "the type's count is wrong";

    /* the rest, from the last one */
    for (int i = TEST_OBJS - 1; i >= 0; --i) {
        kfree(obj[i]);
        obj[i] = NULL;
    }

    if (M_KVMEM_TEST.nr || M_KVMEM_TEST.total)
        return "the type still counts objects";

    if (kvmem.inuse != inuse || kvmem_obj_cnt != objs)
        return "the arena isn't as it was";

    if (kvmem_segs() != segs)
        return "free segments didn't merge";

    return NULL;
}

/**
 * Runs the allocator's self-test, for the `kvmem.test` kernel argument,
 * and prints whether it passed.
 */
void kvmem_selftest(void)
{
    const char *err = kvmem_selftest_run();

    if (err)
        printk("kvmem: self-test failed: %s\n", err);
    else
        printk("kvmem: self-test passed\n");
}

/**
 * Writes one byte past the end of an object, for the `kvmem.guard` kernel
 * argument: the guard page should fault, and the kernel panic.
 */
void kvmem_guardtest(void)
{
    volatile char *buf = kmalloc(3000, &M_KVMEM_TEST, 0);
    volatile char *past = buf + PAGE_ROUND(3000);

    printk("kvmem: writing past %p's end, at %p\n", buf, past);
    *past = 1;

    printk("kvmem: the guard page didn't fault\n");
}
