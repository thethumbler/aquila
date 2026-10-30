/** \file
 * Physical memory: setting it up at boot, and the pages of it.
 *
 * Every page of RAM has a `struct vm_page`, in `pages[]`, with its
 * reference count. `mm_page_alloc()` takes free pages from the buddy
 * allocator (`mm/buddy.c`), and `mm_setup()` sets both up from the boot
 * loader's memory map. See docs/kernel/memory.md.
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/arch.h>
#include <core/string.h>

#include <boot/boot.h>

#include <mm/pmap.h>
#include <mm/mm.h>
#include <mm/vm.h>
#include <mm/buddy.h>

#include <sys/sched.h>

/* a page structure for every page of RAM, up to its end (mm_setup) */
static struct vm_page *pages;
static size_t pages_nr;
#define PAGE(addr)    (pages[(addr)/PAGE_SIZE])

/* pages[] tracks memory, not what is past it (device memory, such as a
 * framebuffer mapped by a process): no reference counts for those */
#define TRACKED(addr) ((addr)/PAGE_SIZE < pages_nr)

/**
 * Adds a reference to a page of RAM. A page past RAM (device memory, such
 * as a framebuffer) has no reference count, and is left alone.
 *
 * @param paddr  the page's physical address
 */
void mm_page_incref(paddr_t paddr)
{
    if (TRACKED(paddr))
        PAGE(paddr).ref++;
}

/**
 * Drops a reference to a page of RAM. A page past RAM is left alone.
 *
 * @param paddr  the page's physical address
 */
void mm_page_decref(paddr_t paddr)
{
    if (TRACKED(paddr))
        PAGE(paddr).ref--;
}

/**
 * Returns how many references a page of RAM has.
 *
 * @param paddr  the page's physical address
 * @return its reference count, or 0 for a page past RAM
 */
size_t mm_page_ref(paddr_t paddr)
{
    return TRACKED(paddr) ? PAGE(paddr).ref : 0;
}

/**
 * Allocates a free page of physical memory.
 *
 * Running out of memory is an error that the caller handles like any
 * other, not a panic: a process's page fault kills it, a system call
 * returns `ENOMEM`, a network frame is dropped. Only what the kernel
 * can't boot without panics.
 *
 * @return the page's structure, zeroed but for its address, or NULL if
 *         there is no free page
 */
struct vm_page *mm_page_alloc(void)
{
    /* a page above 16 MiB, or below once there's none left there (nothing
     * needs memory an ISA device can reach) */
    paddr_t paddr = buddy_alloc(BUDDY_ZONE_NORMAL, PAGE_SIZE);

    if (paddr == (paddr_t) -1)
        paddr = buddy_alloc(BUDDY_ZONE_DMA, PAGE_SIZE);

    if (paddr == (paddr_t) -1)
        return NULL;

    struct vm_page *vm_page = &PAGE(paddr);

    memset(vm_page, 0, sizeof(struct vm_page));
    vm_page->paddr = paddr;

    return vm_page;
}

/**
 * Frees a page of physical memory, back to the zone it came from.
 *
 * @param paddr  the page's physical address
 */
void mm_page_dealloc(paddr_t paddr)
{
    /* to the zone it came from (an address past RAM, buddy_free leaves
     * alone) */
    int zone = paddr < buddy_zone_offset[BUDDY_ZONE_NORMAL] ? BUDDY_ZONE_DMA : BUDDY_ZONE_NORMAL;
    buddy_free(zone, paddr, PAGE_SIZE);
}

/**
 * Maps a page into an address space. The page's references are the vm
 * objects' that hold it, not the mappings', so this doesn't add one.
 *
 * @param pmap   the address space's page tables
 * @param vaddr  the virtual address, page-aligned
 * @param paddr  the page's physical address, page-aligned
 * @param flags  the mapping's permissions (`VM_*`)
 * @return 0, `-EINVAL` for an address that can't be mapped page by page,
 *         or `-ENOMEM` if there was no memory for a page table
 */
int mm_page_map(struct pmap *pmap, vaddr_t vaddr, paddr_t paddr, int flags)
{
    return pmap_add(pmap, vaddr, paddr, flags);
}

/**
 * Unmaps a page from an address space.
 *
 * @param pmap   the address space's page tables
 * @param vaddr  the page's virtual address
 * @return 0, or `-EINVAL` if no page is mapped there
 */
int mm_page_unmap(struct pmap *pmap, vaddr_t vaddr)
{
    /* is a page mapped there? */
    paddr_t paddr = arch_page_get_mapping(pmap, vaddr);

    if (paddr) {
        /* the architecture's page tables */
        pmap_remove(pmap, vaddr, vaddr + PAGE_SIZE);
        return 0;
    }

    return -EINVAL;
}

/**
 * Maps a range of an address space, page by page. Pages that are mapped
 * already are left as they are.
 *
 * @param pmap   the address space's page tables
 * @param paddr  the physical address to map, or 0 to map new pages of
 *               memory
 * @param vaddr  the virtual address
 * @param size   the size of the range, in bytes (rounded to whole pages)
 * @param flags  the mappings' permissions (`VM_*`)
 * @return 0, or `-ENOMEM` if there was no memory for a page or a page
 *         table; the pages mapped before that stay mapped
 */
int mm_map(struct pmap *pmap, paddr_t paddr, vaddr_t vaddr, size_t size, int flags)
{
    int alloc = !paddr;

    vaddr_t endaddr = PAGE_ROUND(vaddr + size);
    vaddr = PAGE_ALIGN(vaddr);
    paddr = PAGE_ALIGN(paddr);

    size_t nr = (endaddr - vaddr) / PAGE_SIZE;

    while (nr--) {
        paddr_t phys = arch_page_get_mapping(pmap, vaddr);

        if (!phys) {
            if (alloc) {
                struct vm_page *vm_page = mm_page_alloc();

                if (!vm_page)
                    return -ENOMEM;

                paddr = vm_page->paddr;
            }

            int err = mm_page_map(pmap, vaddr, paddr, flags);

            if (err) {
                if (alloc)
                    mm_page_dealloc(paddr);

                return err;
            }
        }

        vaddr += PAGE_SIZE;
        paddr += PAGE_SIZE;
    }

    return 0;
}

/**
 * Unmaps every page of a range of an address space. The page tables are
 * walked once (`pmap_remove()`), which skips what has no table: a large
 * range with few pages mapped takes little time.
 *
 * @param pmap   the address space's page tables
 * @param vaddr  the range's start
 * @param size   its size, in bytes (rounded to whole pages)
 */
void mm_unmap_full(struct pmap *pmap, vaddr_t vaddr, size_t size)
{
    pmap_remove(pmap, PAGE_ALIGN(vaddr), PAGE_ROUND(vaddr + size));
}

/*
 * Boot memory: the structures whose size depends on how much RAM there is
 * (pages[], the buddy allocator's bitmaps). They go after the kernel,
 * where the boot loader left nothing, and are taken before the buddy
 * allocator knows what is free; then they are marked used. They are
 * reached through the boot mapping (VMA()).
 */
static paddr_t boot_mem_start, boot_mem_next, boot_mem_end;

/**
 * Allocates boot memory, zeroed, before the buddy allocator is set up.
 * Running out panics: the kernel can't boot without it.
 *
 * @param size   the size wanted, in bytes
 * @param align  its alignment, a power of two
 * @return its kernel address
 */
void *mm_boot_alloc(size_t size, size_t align)
{
    paddr_t paddr = (boot_mem_next + align - 1) & ~(paddr_t) (align - 1);

    if (paddr + size > boot_mem_end)
        panic("mm: out of boot memory");

    boot_mem_next = paddr + size;

    void *ptr = (void *) VMA((uintptr_t) paddr);
    memset(ptr, 0, size);
    return ptr;
}

/** Returns the end of whatever the boot loader left in memory (modules,
 * the kernel's symbols, the command line) that overlaps `[start, start +
 * size)`, or 0 if nothing does. */
static paddr_t boot_left(struct boot *boot, paddr_t start, size_t size)
{
    paddr_t end = start + size;

    for (int i = 0; i < boot->modules_count; ++i) {
        paddr_t s = boot->modules[i].paddr;
        paddr_t e = s + boot->modules[i].size;

        if (start < e && end > s)
            return e;
    }

    /* the kernel's symbols and string table, the command line */
    paddr_t r[3][2] = {{0, 0}, {0, 0}, {0, 0}};

    if (boot->symtab) {
        r[0][0] = LMA((uintptr_t) boot->symtab->sh_addr);
        r[0][1] = r[0][0] + boot->symtab->sh_size;
    }

    if (boot->strtab) {
        r[1][0] = LMA((uintptr_t) boot->strtab->sh_addr);
        r[1][1] = r[1][0] + boot->strtab->sh_size;
    }

    if (boot->cmdline) {
        r[2][0] = LMA((uintptr_t) boot->cmdline);
        r[2][1] = r[2][0] + strlen(boot->cmdline) + 1;
    }

    for (int i = 0; i < 3; ++i)
        if (r[i][1] && start < r[i][1] && end > r[i][0])
            return r[i][1];

    return 0;
}

/** Returns whether `[start, start + size)` is all in one usable region of
 * the boot loader's memory map. */
static int boot_usable(struct boot *boot, paddr_t start, size_t size)
{
    for (int i = 0; i < boot->mmap_count; ++i)
        if (boot->mmap[i].type == MMAP_USABLE &&
            boot->mmap[i].start <= start && start + size <= boot->mmap[i].end)
            return 1;

    return 0;
}

/** Places the boot memory: at the first place after the kernel that is
 * RAM, free, and in the boot mapping. Panics if there's none. */
static void boot_mem_place(struct boot *boot, size_t size)
{
    extern char kernel_end;
    paddr_t start = PAGE_ROUND((uintptr_t) &kernel_end), end;

    while ((end = boot_left(boot, start, size)))
        start = PAGE_ROUND(end);

    if (start + size > ARCH_BOOT_MAPPED || !boot_usable(boot, start, size))
        panic("mm: no room for the page structures after the kernel");

    boot_mem_start = boot_mem_next = start;
    boot_mem_end = start + size;
}

/** Marks as used what isn't usable RAM below `ram_end`: the regions the
 * memory map reserves, and the gaps between its regions. */
static void mm_mark_unusable(struct boot *boot, paddr_t ram_end)
{
    paddr_t at = 0;

    while (at < ram_end) {
        /* the usable region that starts the lowest at or after `at` */
        paddr_t next = ram_end, next_end = ram_end;

        for (int i = 0; i < boot->mmap_count; ++i) {
            mmap_t *m = &boot->mmap[i];

            if (m->type != MMAP_USABLE || m->end <= at)
                continue;

            paddr_t s = m->start < at ? at : m->start;

            if (s < next) {
                next = s;
                next_end = m->end;
            }
        }

        if (next > at)
            buddy_set_unusable(at, next - at);

        at = next_end;
    }

    for (int i = 0; i < boot->mmap_count; ++i)
        if (boot->mmap[i].type != MMAP_USABLE && boot->mmap[i].start < ram_end)
            buddy_set_unusable(boot->mmap[i].start,
                MIN(boot->mmap[i].end, ram_end) - boot->mmap[i].start);
}

/**
 * Sets up physical memory at boot, from the boot loader's memory map: the
 * page structures, the buddy allocator, and what isn't free (the kernel,
 * the modules, the reserved regions, page 0). Then the architecture sets
 * up its page tables (`arch_mm_setup()`).
 *
 * @param boot  what the boot loader passed (see `boot/boot.h`)
 */
void mm_setup(struct boot *boot)
{
    /* the end of RAM, and how much there is */
    paddr_t ram_end = 0;
    uint64_t ram = 0;

    for (int i = 0; i < boot->mmap_count; ++i) {
        if (boot->mmap[i].type == MMAP_USABLE) {
            ram += boot->mmap[i].end - boot->mmap[i].start;
            ram_end = MAX(ram_end, boot->mmap[i].end);
        }
    }

    /* no memory map: as much as the boot loader says there is, from 0 */
    if (!ram_end) {
        ram_end = (paddr_t) boot->total_mem * 1024;
        ram = ram_end;
    }

#ifdef ARCH_RAM_MAX
    /* only what the kernel maps can be used (i386's direct map) */
    if (ram_end > ARCH_RAM_MAX) {
        ram = 0;

        for (int i = 0; i < boot->mmap_count; ++i)
            if (boot->mmap[i].type == MMAP_USABLE && boot->mmap[i].start < ARCH_RAM_MAX)
                ram += MIN(boot->mmap[i].end, ARCH_RAM_MAX) - boot->mmap[i].start;

        printk("kernel: only the first %ld MiB are used\n", (uint64_t) (ARCH_RAM_MAX >> 20));
        ram_end = ARCH_RAM_MAX;
    }
#endif

    /* the buddy allocator works in blocks of BUDDY_MAX_BS, so round up to
     * the next one; what's past RAM is set unusable (mm_mark_unusable()),
     * and only RAM is handed out (buddy_reserved_done()) */
    ram_end = (ram_end + BUDDY_MAX_BS - 1) & ~(paddr_t) (BUDDY_MAX_BS - 1);

    printk("kernel: %ld MiB of memory, up to %p\n", ram >> 20, ram_end);

    pages_nr = ram_end / PAGE_SIZE;

    /* room for the page structures and the buddy allocator's bitmaps
     * (at most two bits per page, over all its orders) */
    size_t pages_size = pages_nr * sizeof(struct vm_page);
    boot_mem_place(boot, PAGE_ROUND(pages_size + pages_nr / 4 + 0x10000));

    pages = mm_boot_alloc(pages_size, sizeof(uintptr_t));

    buddy_setup(ram_end);

    /* never hand out physical page 0: 0 means "no page" (no mapping) */
    buddy_set_unusable(0, PAGE_SIZE);

    buddy_set_unusable(boot_mem_start, PAGE_ROUND(boot_mem_next) - boot_mem_start);
    mm_mark_unusable(boot, ram_end);

    /* the modules (the RAM disk) */
    for (int i = 0; i < boot->modules_count; ++i) {
        module_t *mod = &boot->modules[i];
        buddy_set_unusable(mod->paddr, mod->size);
    }

    /* what /proc/meminfo reports as the total */
    extern size_t k_total_mem;
    k_total_mem = ram;

    arch_mm_setup();
}
