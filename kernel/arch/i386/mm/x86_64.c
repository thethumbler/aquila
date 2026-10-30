/** \file
 * x86_64: page tables, with 4-level paging.
 *
 * All of physical memory is mapped at `ARCH_DMAP_BASE` (the direct map),
 * and page tables and pages are reached there. Every address space has
 * its own PML4. The PDPTs of the kernel's half (entries 256 to 511) are
 * all made at boot and shared by every PML4, so the kernel's mappings are
 * the same in every address space, and a PML4's kernel entries never
 * change. Switching address spaces is loading CR3.
 *
 * References:
 *      [1] - Intel(R) 64 and IA-32 Architectures Software Developer's
 *            Manual, Volume 3 (System Programming Guide), Chapter 4
 *            (Paging), 4.5 (4-Level Paging)
 */

#include <core/arch.h>
#include <core/panic.h>
#include <core/printk.h>
#include <core/string.h>
#include <core/system.h>
#include <boot/boot.h>
#include <cpu/cpu.h>
#include <sys/cpu.h>

#include <mm/pmap.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/vm.h>

MALLOC_DEFINE(M_PMAP, "pmap", "physical memory map structure");

extern struct boot *__kboot;

#define PG_PRESENT  0x001
#define PG_WRITE    0x002
#define PG_USER     0x004
#define PG_PWT      0x008   /* write-through */
#define PG_PCD      0x010   /* cache disabled */
#define PG_LARGE    0x080   /* a large page: 2 MiB in a PD, 1 GiB in a PDPT */
#define PG_GLOBAL   0x100

#define PG_ADDR(e)  ((e) & 0x000FFFFFFFFFF000ULL)

/* the index of `va` in its table at `level` (0: PT, 1: PD, 2: PDPT, 3: PML4) */
#define IDX(va, level)  (((va) >> (12 + 9 * (level))) & 0x1FF)

#define KERNEL_PML4E    256
#define LARGE_SIZE      0x200000ULL

static struct pmap k_pmap;

/* the end of the direct map: the end of RAM, rounded up to 2 MiB
 * (pmap_init) */
static paddr_t dmap_end;

/* the running CPU's page tables, which its CR3 has */
#define cur_pmap (curcpu()->arch.pmap)

/** Returns a page table's kernel address, in the direct map. */
static inline uint64_t *table(paddr_t paddr)
{
    return PHYS2VIRT(paddr);
}

/** Allocates an empty page table. Returns its physical address, or 0 if
 * there is no memory for it. */
static paddr_t table_alloc(void)
{
    struct vm_page *vm_page = mm_page_alloc();

    if (!vm_page)
        return 0;

    memset(table(vm_page->paddr), 0, PAGE_SIZE);
    return vm_page->paddr;
}

/** Allocates a page table that the kernel can't boot without: it panics
 * if there is no memory. */
static paddr_t table_alloc_boot(void)
{
    paddr_t paddr = table_alloc();

    if (!paddr)
        panic("no memory for the kernel's page tables");

    return paddr;
}

/** Frees a page table. */
static void table_free(paddr_t paddr)
{
    mm_page_dealloc(paddr);
}

/** Returns the page table entry that maps `va` in `pmap`. With `make`, the
 * tables on the way are made if they're missing (NULL if there's no memory
 * for one); without it, NULL if one is missing. Also NULL in the direct
 * map's large pages. */
static uint64_t *pte_get(struct pmap *pmap, vaddr_t va, int make)
{
    uint64_t *t = table(pmap->map);
    uint64_t flags = PG_PRESENT | PG_WRITE | (IDX(va, 3) < KERNEL_PML4E ? PG_USER : 0);

    for (int level = 3; level > 0; --level) {
        uint64_t *e = &t[IDX(va, level)];

        if (!(*e & PG_PRESENT)) {
            paddr_t t_new;

            if (!make || !(t_new = table_alloc()))
                return NULL;

            *e = t_new | flags;
        }

        /* the direct map's large pages aren't changed page by page */
        if (*e & PG_LARGE)
            return NULL;

        t = table(PG_ADDR(*e));
    }

    return &t[IDX(va, 0)];
}

/** Returns the page table entry that maps `va` in `pmap`, or NULL if a
 * table on the way is missing (or it's in the direct map's large pages).
 * Sets `*next` to the next address to look at: past the range a missing
 * table would map, so that a walk over a large range that is mostly
 * empty skips it a table at a time, not a page at a time. */
static uint64_t *pte_find(struct pmap *pmap, vaddr_t va, vaddr_t *next)
{
    uint64_t *t = table(pmap->map);

    for (int level = 3; level > 0; --level) {
        uint64_t e = t[IDX(va, level)];

        if (!(e & PG_PRESENT) || (e & PG_LARGE)) {
            vaddr_t span = 1ULL << (12 + 9 * level);
            *next = (va & ~(span - 1)) + span;
            return NULL;
        }

        t = table(PG_ADDR(e));
    }

    *next = va + PAGE_SIZE;
    return &t[IDX(va, 0)];
}

/*
 * Switches the running CPU to an address space's page tables.
 */
struct pmap *pmap_switch(struct pmap *pmap)
{
    if (!pmap)
        panic("pmap?");

    struct pmap *ret = cur_pmap;

    if (pmap != cur_pmap) {
        pmap_loaded(cur_pmap, pmap);
        write_cr3(pmap->map);
        cur_pmap = pmap;
    }

    return ret;
}

/** Returns the end of the RAM in the boot loader's memory map. */
static paddr_t ram_end(void)
{
    paddr_t end = 0;

    for (int i = 0; i < __kboot->mmap_count; ++i)
        if (__kboot->mmap[i].type == MMAP_USABLE && __kboot->mmap[i].end > end)
            end = __kboot->mmap[i].end;

    return end;
}

/*
 * Takes over the boot page tables as the kernel's, at boot: maps all of
 * RAM in the direct map, with 2 MiB pages, and makes every PDPT of the
 * kernel's half, so that those PML4 entries never change and every
 * address space can share them.
 */
void pmap_init(void)
{
    /* The boot tables (boot/x86_64_bootstrap.S) become the kernel's. They
     * map the kernel at -2 GiB and the first GiB of the direct map (and
     * the same memory at 0, which is removed below). */
    k_pmap.map = read_cr3() & ~PAGE_MASK;
    k_pmap.ref = 1;
    kvm_space.pmap = &k_pmap;
    pmap_loaded(NULL, &k_pmap);
    cur_pmap = &k_pmap;

    uint64_t *pml4 = table(k_pmap.map);

    /* map the rest of RAM in the direct map, with 2 MiB pages */
    paddr_t end = (ram_end() + LARGE_SIZE - 1) & ~(LARGE_SIZE - 1);
    dmap_end = end;

    for (paddr_t pa = 0; pa < end; pa += LARGE_SIZE) {
        vaddr_t va = ARCH_DMAP_BASE + pa;
        uint64_t *t = pml4;

        for (int level = 3; level > 1; --level) {
            uint64_t *e = &t[IDX(va, level)];

            if (!(*e & PG_PRESENT))
                *e = table_alloc_boot() | PG_PRESENT | PG_WRITE;

            t = table(PG_ADDR(*e));
        }

        t[IDX(va, 1)] = pa | PG_PRESENT | PG_WRITE | PG_LARGE | PG_GLOBAL;
    }

    /* make every PDPT of the kernel's half now, before there is any
     * other PML4 to share them */
    for (int i = KERNEL_PML4E; i < 512; ++i)
        if (!(pml4[i] & PG_PRESENT))
            pml4[i] = table_alloc_boot() | PG_PRESENT | PG_WRITE;

    /* remove the mapping at 0: the lower half is user space */
    pml4[0] = 0;
    tlb_flush();

    printk("x86_64: %ld MiB in the direct map\n", (uint64_t) (end >> 20));
}

/** Allocates a `struct pmap`. */
static struct pmap *pmap_alloc(void)
{
    return kmalloc(sizeof(struct pmap), &M_PMAP, 0);
}

/** Frees an address space's PML4, once no CPU has it loaded. */
static void pmap_release(struct pmap *pmap)
{
    /* make sure that no CPU has it loaded anymore */
    tlb_shootdown(pmap, 0, 0, 1);

    if (pmap->map)
        table_free(pmap->map);

    kfree(pmap);
}

/*
 * Makes the page tables of a new address space: an empty user half, and
 * the kernel's half, which every address space shares.
 */
struct pmap *pmap_create(void)
{
    struct pmap *pmap = pmap_alloc();

    if (pmap == NULL)
        return NULL;

    memset(pmap, 0, sizeof(struct pmap));

    if (!(pmap->map = table_alloc())) {
        kfree(pmap);
        return NULL;
    }

    pmap->ref = 1;

    /* the kernel's half, the same in every PML4 */
    uint64_t *pml4 = table(pmap->map), *kpml4 = table(k_pmap.map);

    for (int i = KERNEL_PML4E; i < 512; ++i)
        pml4[i] = kpml4[i];

    return pmap;
}

/*
 * Adds a reference to an address space's page tables.
 */
void pmap_incref(struct pmap *pmap)
{
    pmap->ref++;
}

/*
 * Drops a reference to an address space's page tables, freeing them with
 * the last one.
 */
void pmap_decref(struct pmap *pmap)
{
    if (--pmap->ref == 0)
        pmap_release(pmap);
}

/** Returns the page table entry bits for the `VM_*` flags `flags`. */
static uint64_t pte_flags(uint32_t flags)
{
    uint64_t pte = PG_PRESENT;

    if (flags & (VM_KW | VM_UW))
        pte |= PG_WRITE;

    if (flags & VM_URWX)
        pte |= PG_USER;

    /* device memory: uncached */
    if (flags & VM_NOCACHE)
        pte |= PG_PCD | PG_PWT;

    return pte;
}

/*
 * Maps a page in an address space, replacing what was mapped there.
 */
int pmap_add(struct pmap *pmap, vaddr_t va, paddr_t pa, uint32_t flags)
{
    if ((va & PAGE_MASK) || (pa & PAGE_MASK))
        return -EINVAL;

    /* the direct map's large pages aren't changed page by page */
    if (va >= ARCH_DMAP_BASE && va < ARCH_DMAP_BASE + dmap_end)
        return -EINVAL;

    uint64_t *pte = pte_get(pmap, va, 1);

    /* there was no memory for a page table on the way */
    if (!pte)
        return -ENOMEM;

    uint64_t old = *pte;
    *pte = pa | pte_flags(flags);

    /* another page was mapped there: flush it from the TLBs */
    if (old & PG_PRESENT)
        tlb_shootdown(pmap, va, va + PAGE_SIZE, 0);

    return 0;
}

/*
 * Unmaps the pages of a range of an address space, and flushes them from
 * the TLBs that may have them.
 */
void pmap_remove(struct pmap *pmap, vaddr_t sva, vaddr_t eva)
{
    if ((sva & PAGE_MASK) || (eva & PAGE_MASK))
        return;

    vaddr_t first = eva, last = sva;

    for (vaddr_t va = sva, next; va < eva && va >= sva; va = next) {
        uint64_t *pte = pte_find(pmap, va, &next);

        if (pte && (*pte & PG_PRESENT)) {
            *pte = 0;
            first = MIN(first, va);
            last = va + PAGE_SIZE;
        }
    }

    if (first < last)
        tlb_shootdown(pmap, first, last, 0);
}

/** Frees a page table at `level` and the tables under it. The pages they
 * map lose the reference that the page fault handler gave them
 * (`mm_page_incref()`). */
static void tables_free(paddr_t paddr, int level)
{
    uint64_t *t = table(paddr);

    for (int i = 0; i < 512; ++i) {
        if (!(t[i] & PG_PRESENT))
            continue;

        if (level > 1)
            tables_free(PG_ADDR(t[i]), level - 1);
        else
            mm_page_decref(PG_ADDR(t[i]));
    }

    table_free(paddr);
}

/*
 * Frees the page tables of an address space's user half. The pages they
 * map lose the reference that the page fault handler gave them
 * (`mm_page_incref()`).
 */
void pmap_remove_all(struct pmap *pmap)
{
    uint64_t *pml4 = table(pmap->map);

    for (int i = 0; i < KERNEL_PML4E; ++i) {
        if (pml4[i] & PG_PRESENT) {
            tables_free(PG_ADDR(pml4[i]), 3);
            pml4[i] = 0;
        }
    }

    tlb_shootdown(pmap, 0, USER_STACK, 0);
}

/*
 * Changes the permissions of the mapped pages of a range of an address
 * space.
 */
void pmap_protect(struct pmap *pmap, vaddr_t sva, vaddr_t eva, uint32_t prot)
{
    if (sva & PAGE_MASK)
        return;

    vaddr_t first = eva, last = sva;

    for (vaddr_t va = sva, next; va < eva && va >= sva; va = next) {
        uint64_t *pte = pte_find(pmap, va, &next);

        if (pte && (*pte & PG_PRESENT)) {
            *pte = PG_ADDR(*pte) | pte_flags(prot);
            first = MIN(first, va);
            last = va + PAGE_SIZE;
        }
    }

    if (first < last)
        tlb_shootdown(pmap, first, last, 0);
}

void pmap_copy(struct pmap *dst_map, struct pmap *src_map, vaddr_t dst_addr, size_t len,
 vaddr_t src_addr)
{
    return;
}

/* Does nothing: changes take effect at once on x86. */
void pmap_update(struct pmap *pmap)
{
    return;
}

/*
 * Copies a page of physical memory to another, through the direct map.
 */
void pmap_page_copy(paddr_t src, paddr_t dst)
{
    memcpy(PHYS2VIRT(dst), PHYS2VIRT(src), PAGE_SIZE);
}

/* Does nothing: not implemented on x86, nothing uses it. */
void pmap_page_protect(struct vm_page *pg, uint32_t flags)
{
    return;
}

/* Not implemented on x86, nothing uses it: returns -1. */
int pmap_clear_modify(struct vm_page *pg)
{
    return -1;
}

/* Not implemented on x86, nothing uses it: returns -1. */
int pmap_clear_reference(struct vm_page *pg)
{
    return -1;
}

/* Not implemented on x86, nothing uses it: returns -1. */
int pmap_is_modified(struct vm_page *pg)
{
    return -1;
}

/* Not implemented on x86, nothing uses it: returns -1. */
int pmap_is_referenced(struct vm_page *pg)
{
    return -1;
}

/*
 * Finds the page mapped at a virtual address of an address space.
 */
paddr_t arch_page_get_mapping(struct pmap *pmap, vaddr_t vaddr)
{
    uint64_t *pte = pte_get(pmap, vaddr, 0);

    return pte && (*pte & PG_PRESENT) ? PG_ADDR(*pte) : 0;
}

/*
 * Unmaps a page of the running CPU's address space.
 */
int arch_page_unmap(vaddr_t vaddr)
{
    if (vaddr & PAGE_MASK)
        return -EINVAL;

    pmap_remove(cur_pmap, vaddr, vaddr + PAGE_SIZE);
    return 0;
}

/*
 * Handles a page fault: turns the CPU's error code into the kernel's
 * `PF_*` flags, and calls `mm_page_fault()`.
 */
int arch_mm_page_fault(vaddr_t vaddr, int err)
{
    int flags = 0;

    flags |= err & 0x01? PF_PRESENT : 0;
    flags |= err & 0x02? PF_WRITE   : PF_READ;
    flags |= err & 0x04? PF_USER    : 0;
    flags |= err & 0x10? PF_EXEC    : 0;

    return mm_page_fault(vaddr, flags);
}

/*
 * Reads from a page of physical memory, through the direct map.
 */
int pmap_page_read(paddr_t paddr, off_t off, size_t size, void *buf)
{
    if (off < 0 || off >= PAGE_SIZE)
        return -EINVAL;

    memcpy(buf, (char *) PHYS2VIRT(paddr) + off, MIN(size, PAGE_SIZE - (size_t) off));
    return 0;
}

/*
 * Writes to a page of physical memory, through the direct map.
 */
int pmap_page_write(paddr_t paddr, off_t off, size_t size, void *buf)
{
    if (off < 0 || off >= PAGE_SIZE)
        return -EINVAL;

    memcpy((char *) PHYS2VIRT(paddr) + off, buf, MIN(size, PAGE_SIZE - (size_t) off));
    return 0;
}

/* where the window for devices' registers has room next (all of RAM,
 * and what the boot loader left in it, is in the direct map) */
static vaddr_t mmio_next = ARCH_MMIO_BASE;

/** Maps a range of physical memory in the window, with `flags`. Returns
 * its kernel address, or NULL if there's no room left in the window, or no
 * memory for a page table. The window's addresses are never reused. */
static void *window_map(uintptr_t paddr, size_t size, uint32_t flags)
{
    paddr_t start = PAGE_ALIGN(paddr);
    size_t  len   = PAGE_ROUND(paddr + size) - start;

    if (mmio_next + len > ARCH_MMIO_BASE + ARCH_MMIO_SIZE)
        return NULL;    /* no room left in the window */

    vaddr_t vaddr = mmio_next;

    /* if there's no memory for a page table, unmap what was mapped */
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        if (pmap_add(&k_pmap, vaddr + off, start + off, flags)) {
            pmap_remove(&k_pmap, vaddr, vaddr + off);
            return NULL;
        }
    }

    mmio_next += len;

    return (void *) (vaddr + (paddr - start));
}

/*
 * Returns a kernel address for a range of physical memory: its address
 * in the direct map if it's there, else a mapping made in the window.
 */
void *arch_phys_map(uintptr_t paddr, size_t size)
{
    if (paddr + size <= dmap_end)
        return PHYS2VIRT(paddr);

    return window_map(paddr, size, VM_KRW);
}

/*
 * Maps a device's registers in the kernel's address space, uncached.
 */
void *arch_mmio_map(uintptr_t paddr, size_t size)
{
    return window_map(paddr, size, VM_KRW | VM_NOCACHE);
}
