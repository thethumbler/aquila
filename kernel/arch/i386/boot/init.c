/** \file
 * i386: the kernel's first steps in C, moving itself to the higher half.
 *
 * The boot code (`multiboot.S`) calls `x86_bootstrap()` with paging off.
 * It builds boot page tables that map the start of physical memory twice,
 * at 0 and at the kernel's virtual base (3 GiB), turns paging on, moves
 * the stack to the higher half, and goes on to `x86_cpu_init()`.
 *
 * Only i386 builds this file: x86_64 does the same in assembly
 * (`x86_64_bootstrap.S`).
 */

#include <core/system.h>
#include <cpu/cpu.h>
#include <mm/mm.h>
#include <boot/multiboot.h>
#include <boot/boot.h>

/* The boot CPU's first page directory, just enough to map the kernel. */
volatile uint32_t _BSP_PD[1024]  __aligned(PAGE_SIZE);

#define P   _BV(0)
#define RW  _BV(1)
#define PCD _BV(4)

extern char kernel_end;
char scratch[1024 * 1024] __aligned(PAGE_SIZE); /* 1 MiB for the boot page tables */

/** Loads the page directory into CR3, and turns paging on. */
static inline void enable_paging(uintptr_t page_directory)
{
    write_cr3(page_directory);
    uint32_t cr0 = read_cr0();

    /* without WP the kernel writes through read-only pages, e.g. into a
     * copy-on-write page still shared with another process after fork */
    write_cr0(cr0 | CR0_PG | CR0_WP);
}

/** Builds the boot page tables, which map the first `ARCH_BOOT_MAPPED`
 * bytes of physical memory both at 0 and at the kernel's virtual base,
 * and switches to them. */
static void switch_to_higher_half(void)
{
    uint32_t i;
    uintptr_t entries;

    /* clear the page directory (a loop: memset() is in the higher half,
     * which isn't mapped yet) */
    for (i = 0; i < 1024; ++i)
        _BSP_PD[i] = 0;

    /* how many page tables it takes to map the kernel and what follows */
    entries = ARCH_BOOT_MAPPED / TABLE_SIZE;

    uint32_t *_BSP_PT = (uint32_t *) scratch;

    /* each page mapped to itself */
    for (i = 0; i < entries * 1024; ++i)
        _BSP_PT[i] = (i * PAGE_SIZE) | P | RW;

    /* the tables at 0, where the code runs now */
    for (i = 0; i < entries; ++i)
        _BSP_PD[i] = ((uint32_t) _BSP_PT + i * PAGE_SIZE) | P | RW;

    /* the same tables in the higher half (3 GiB), where it runs next */
    for (i = 0; i < entries; ++i)
        _BSP_PD[768 + i] = ((uint32_t) _BSP_PT + i * PAGE_SIZE) | P | RW;

    /* turn paging on with the boot CPU's page directory */
    enable_paging((uintptr_t) _BSP_PD);
}

/**
 * Moves the kernel to the higher half and goes on to `x86_cpu_init()`.
 * The boot code calls it; it doesn't return.
 */
void x86_bootstrap()
{
    /* The segments are the boot loader's until x86_cpu_init() loads our
     * GDT: Multiboot leaves them flat, 0 to 4 GiB (x86_64's bootstrap
     * loads its own). First, map the kernel in the higher half. */
    switch_to_higher_half();

    /* move the stack pointer to the higher half too */
    extern void early_init_fix_stack(uintptr_t);
    early_init_fix_stack(VMA((uintptr_t) 0));

    /* set up the CPU and go on to the kernel */
    extern void x86_cpu_init(void);
    x86_cpu_init();

    /* not reached; halt if it ever is */
    for (;;)
        asm volatile ("hlt;");
}
