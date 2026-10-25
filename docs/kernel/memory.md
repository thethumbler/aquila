# Memory

A kernel manages memory at two levels. Underneath is *physical memory*:
the RAM chips, cut into pages of 4 KiB, which the kernel hands out to
whoever needs one. On top are *address spaces*: each process sees memory
from address 0 to the top of user space as if it had it all to itself,
and the kernel decides, page by page, which physical page (if any) is
behind each of its addresses. The *MMU*, the part of the CPU that
translates addresses, does the lookup on every access, from *page tables*
the kernel fills in.

Most of that is design and the same on any machine: which pages are
free, what a process's memory is made of, when a page is copied. The
page tables' format and how the MMU reports a fault are the
architecture's. This document describes the kernel's side first, then
how x86 and aarch64 do their part (see
[The kernel and the machine](architecture.md) for the split).

## The kernel's side

### The pieces

| Piece                    | What it does                                        | Where                     |
|--------------------------|-----------------------------------------------------|---------------------------|
| `pages[]`, `struct vm_page` | one record for each physical page               | [`mm/mm.c`](../../kernel/mm/mm.c) |
| the buddy allocator      | which physical pages are free                       | [`mm/buddy.c`](../../kernel/mm/buddy.c) |
| `kmalloc()`, `kfree()`   | the kernel's own objects                            | [`mm/kvmem.c`](../../kernel/mm/kvmem.c), [`mm/slab.c`](../../kernel/mm/slab.c), [`mm/vmem.c`](../../kernel/mm/vmem.c) |
| `struct vm_space`        | an address space: a list of regions and the page tables | [`mm/vm_space.c`](../../kernel/mm/vm_space.c) |
| `struct vm_entry`        | a region: where, how big, which permissions, what's behind it | `kernel/include/mm/vm.h` |
| `struct vm_anon`, `struct vm_aref` | a region's private pages              | [`mm/vm_anon.c`](../../kernel/mm/vm_anon.c) |
| `struct vm_object`       | a file's pages, shared by everyone who maps it      | [`fs/vm_object.c`](../../kernel/fs/vm_object.c) |
| `mm_page_fault()`        | puts a page behind an address when it's first used  | [`mm/fault.c`](../../kernel/mm/fault.c) |
| `pmap_*()`               | the page tables: the architecture's                 | `kernel/include/mm/pmap.h` |

### Physical memory

At boot `mm_setup()` takes over the memory the boot loader reported (see
[Booting](booting.md)). It keeps a `struct vm_page` for every page of
RAM in one array, `pages[]`, indexed by physical address. A page's record
says where it is and, for a file's page, which object and offset it
holds.

Which pages are free is the **buddy allocator**'s business. It keeps RAM
as blocks of 2^*n* pages, *n* from 0 (4 KiB) to 10 (4 MiB), one bitmap
per size. Asking for a page takes a free one of that size, or splits a
bigger block in two "buddies", one used and one free, as often as
needed; freeing a page merges it with its buddy when that one is free
too, and so on up. Both take a few steps whatever the amount of RAM, and
big blocks stay together. AquilaOS only ever asks it for single pages
(`mm_page_alloc()`).

Shortcuts:

- RAM is in two *zones*, below 16 MiB (`BUDDY_ZONE_DMA`, for old ISA
  devices that can't reach higher) and above (`BUDDY_ZONE_NORMAL`). Pages
  come from the second, and from the first only once the second has none
  left (`mm_page_alloc()`).
- What isn't free at boot (the kernel, the modules, reserved ranges) is
  marked used in whole 4 MiB blocks; the first allocation gives back the
  pages of those blocks that no reserved range covers
  (`buddy_reserved_done()`). The end of RAM is rounded down to 4 MiB:
  what's past it is lost.
- There is no reclaiming: pages aren't swapped out, cached file pages
  aren't dropped.

**Out of memory is an error**, not a panic, as in xv6: `mm_page_alloc()`
returns `NULL`, and each caller fails the way it can:

- a page fault that finds no memory kills the process (`SIGKILL`): it
  couldn't go on, the access would fault again. If the fault came from a
  system call copying to or from the process's memory, the copy fails
  (`EFAULT`) and the process is killed on its way back;
- `fork()`, `mmap()` and the others return `ENOMEM`: a page table, a
  map entry or a structure they need couldn't be had;
- `kmalloc()` returns `NULL` (the slab allocator, the page-granular one);
- the network drops a frame it has no buffer for;
- the kernel panics only for what it can't boot without: its own page
  tables, a driver's rings, the RAM disk's mapping.

Each step of a page fault takes its page, its mapping and its place in
the anon (below) before it changes anything shared, and undoes what it
did if one of them fails; a page is zeroed before any process can see
it. The `oom` test (`tests/oom/`) runs out of memory on purpose, from a
page fault and from inside `read()`, three times each: the process is
killed, the memory comes back, the system goes on.

`/proc/meminfo` shows the total and what the allocator has left:

```
MemTotal: 261631 kB
MemFree: 189219 kB
KVMemUsed: 416 KB
KVMemObjCnt: 48
Slab: 612 kB
```

### The kernel's own memory

The kernel needs memory for its structures: processes, open files,
vnodes, network buffers. `kmalloc(size, type, flags)` gives it, and
`kfree()` takes it back. Like FreeBSD's `malloc(9)`, every allocation
names a *malloc type*, declared once with `MALLOC_DEFINE()`:

```c
MALLOC_DEFINE(M_VM_ENTRY, "vm-entry", "virtual memory map entry");

struct vm_entry *e = kmalloc(sizeof(struct vm_entry), &M_VM_ENTRY, M_ZERO);
```

Each type counts its objects and bytes, and `/proc/kvmem` lists them, so
a leak shows up as one type's count that only grows. `M_ZERO` asks for
zeroed memory. `kmalloc()` returns `NULL` when its region of addresses
is full, or RAM is.

Behind `kmalloc()` are two allocators:

- **Slabs**, for objects of up to 1 KiB (`SLAB_MAX`), most of them. A
  slab is one page of objects of one size, a power of two from 16 bytes,
  and of one malloc type. Each type keeps, for each size, a list of its
  slabs that have free objects, and each slab a list of its free objects:
  allocating and freeing are a few pointer moves. A slab whose objects
  are all free gives its page back.
- **kvmem**, for bigger objects, which get whole pages of their own. A
  *vmem arena* (Bonwick and Adams's allocator of ranges) hands out a range
  of the kernel's addresses, pages are allocated and mapped there, and
  the range ends in a **guard page**, never mapped: an overflow past the
  end of a buffer, or of a kernel stack, faults instead of overwriting the
  next object. It costs addresses, not memory.

The two live in their own regions of the kernel's address space
(`ARCH_SLAB_BASE`, `ARCH_KVMEM_BASE`), so `kfree()` knows from an
address which one it came from. vmem's own records are small objects,
from the slabs; the slabs take their pages straight from the buddy
allocator. Nothing goes in a circle.

The `kvmem.test` and `kvmem.guard` arguments run a self-test of both at
boot (the `kvmem` test of [Building and testing](building.md)).

### Address spaces

Each process has a `struct vm_space`: its page tables (`pmap`, the
architecture's) and a list of regions, `struct vm_entry`, sorted by
address. A region says:

- where it is (`base`, `size`) and what's allowed (`VM_UR`, `VM_UW`,
  `VM_UX`);
- what's behind it: a file (`vm_object`, and the offset in it), or
  nothing (zero-filled memory);
- its private pages (`vm_anon`), the ones this process wrote to.

A program after `exec` has these regions (see [Processes and threads](processes.md)):

| Region        | Behind it                                   | Made by              |
|---------------|---------------------------------------------|----------------------|
| text          | the program's file, read-only, shared       | the ELF loader       |
| data          | the program's file, copied when touched     | the ELF loader       |
| bss           | zeros                                       | the ELF loader       |
| heap          | zeros, grown by `sbrk()`                    | `binfmt_load()`      |
| `mmap()`s     | a file, or zeros                            | `mmap()`             |
| the sigcode   | a page of code to return from signal handlers | `arch_sigcode_map()` |
| the stack     | zeros, 8 MiB (`USER_STACK_SIZE`), at the top of user space | `binfmt_load()` |

Nothing is loaded or allocated when a region is made: pages come when an
address is first used, at a page fault. A program that maps a file of a
gigabyte and reads one page of it uses one page of memory.

### Page faults

When a process touches an address whose page isn't mapped, or writes to
one that is read-only, the MMU faults and the architecture calls
`mm_page_fault(address, flags)`, the flags saying whether it was a read,
a write or an execution, and whether a page was there. It finds the
region with the address (none, or one that doesn't allow the access:
`-EFAULT`, and the architecture turns that into `SIGSEGV` or a failed
copy, see [Interrupts](interrupts.md)), then tries, in order:

1. **A page is there, but read-only** (`pf_present()`): if the page is
   this process's alone, it was made read-only by `fork()`; it's made
   writable again, or, if it's marked to be copied before it's written
   (`VM_COPY`), it's copied first.
2. **The region's private pages** (`pf_anon()`): if it has one for this
   offset, a read maps it read-only, a write maps it writable if this
   region is its only user, or copies it first (*copy on write*).
3. **The file behind the region** (`pf_object()`): the file's page is
   looked up in its `vm_object`, read from the file if it isn't there yet
   (`vnode_page_in()`). A read-only region maps it as it is, shared with
   every process that maps the file; a writable one gets a private copy,
   which goes into its `vm_anon`.
4. **Nothing behind it** (`pf_zero()`): a new page, zeroed, goes into the
   region's `vm_anon`.

### fork and copy on write

`fork()` doesn't copy a process's memory. `vm_space_fork()` copies the
list of regions: the child's regions point to the same `vm_object`s and
the same `vm_anon`s, whose reference counts go up, and every writable
region of the parent is made read-only in its page tables. The child
starts with empty page tables.

The first fault in either process on a shared `vm_anon` gives it its own
copy of the anon: a new table of the same pages, each page's reference
count (`vm_aref->ref`) one higher. A read then maps the page read-only;
a write to a page whose count is above 1 copies it, and the copy is this
process's alone. A page is only ever copied when someone writes to it,
and only by the writer.

This is the design of the BSDs' UVM (Chuck Cranor's), smaller: a
`vm_anon` is UVM's *amap*, a `vm_aref` its *anon*.

### mmap and sbrk

- **`sbrk()`** grows or shrinks the heap region, up to the next region.
  The new pages are zero-filled at their first use.
- **`mmap()`** adds a region: a file's (`MAP_PRIVATE`: its pages are
  copied when written), a device's (`MAP_SHARED` on a device: the
  framebuffer maps its memory into the process, `kdev_map()`), or
  anonymous memory. Without `MAP_FIXED` it goes in the highest gap that
  fits, which is right below the sigcode page and the stack.
- **`munmap()`** removes a region.

Shortcuts: `munmap()` only removes a whole region, exactly as it was
mapped; `MAP_FIXED` can't replace a mapping that's there; anonymous
memory can't be `MAP_SHARED` (it would need a page shared between
processes that `fork()` doesn't copy); a regular file can't be mapped
`MAP_SHARED` (no filesystem implements it, so writes would never reach
the file); there is no `mprotect()`. Regions are a list, searched from
the start at each fault: fine for the few regions a program has here.

### The kernel's address space

The kernel has an address space too, `kvm_space`, whose mappings are in
the upper half of every process's address space, so the kernel's
addresses work whichever process runs (x86 shares the tables in every
process's, aarch64 has one table of its own for that half). It holds:

- the **direct map**: all of RAM, at `PHYS2VIRT(paddr)`. The kernel
  reaches any physical page there (page tables, a page being copied, a
  file's page) without mapping it;
- the kernel itself;
- the slabs' and kvmem's regions;
- the windows for what isn't RAM: a device's registers
  (`arch_mmio_map()`, uncached), ACPI's tables, the framebuffer.

The architecture decides where each one is (`kernel/arch/i386/include/mm/mm.h`,
`kernel/arch/aarch64/include/mm/mm.h`).

### The page table interface

The kernel changes page tables only through the `pmap_*()` functions
(the name is the BSDs'), which the architecture implements:

| Function                | What                                                  |
|-------------------------|-------------------------------------------------------|
| `pmap_create()`         | new tables for a process, the kernel's part included  |
| `pmap_switch()`         | load another address space on this CPU                |
| `pmap_add()`            | map a page, with `VM_*` permissions                    |
| `pmap_remove()`         | unmap a range                                         |
| `pmap_protect()`        | change a range's permissions                          |
| `pmap_remove_all()`     | the whole user part goes (exit, exec)                 |
| `pmap_page_copy()`, `pmap_page_read()`, `pmap_page_write()` | a physical page's contents |
| `arch_page_get_mapping()` | which physical page is at an address                |

The architecture keeps every CPU's TLB right when a mapping changes; see
[SMP and the kernel lock](smp.md).

## x86: page tables

### 32-bit paging (i386)

On i386 an address has three parts: 10 bits pick an entry in the *page
directory* (one per address space, whose physical address is in `CR3`),
10 bits an entry in the *page table* it points to, and 12 bits the byte
in the 4 KiB page. A directory covers 4 GiB with 1024 tables of 4 MiB.
Each entry has the page's physical address and bits: present, writable,
user (accessible from user mode), cache disabled.

### 4-level paging (x86_64)

x86_64 addresses are 48 bits used of 64, the rest copies of bit 47: the
lower half (from 0) and the upper half (up to the top) are usable, the
middle isn't. Four levels of tables of 512 entries each, the *PML4*, the
*PDPT*, the *page directory* and the *page table*, with 9 bits of the
address for each. A PD entry can map a 2 MiB page directly
(`PG_LARGE`), which the direct map uses.

### The kernel's half

In every address space, the kernel's half is the same: on i386 the
directory's entries from 768 (`0xC0000000`), on x86_64 the PML4's from
256. `pmap_init()` makes all the tables below those entries at boot, and
`pmap_create()` copies the entries into each new address space. The
kernel's mappings then change only in tables every address space shares,
never in a top-level entry: a kernel mapping made while one process runs
is there in all of them.

### Layouts

i386 (`ARCH_*` in `kernel/arch/i386/include/mm/mm.h`):

| From          | Size     | What                                                    |
|---------------|----------|---------------------------------------------------------|
| `0x00000000`  | 3 GiB    | user space: programs at `0x08048000`, the stack below `0xC0000000` |
| `0xC0000000`  | 768 MiB  | the direct map (the kernel at `0xC0100000`, in it)      |
| `0xF0000000`  | 64 MiB   | kvmem                                                   |
| `0xF4000000`  | 64 MiB   | slabs                                                   |
| `0xF8000000`  |          | the framebuffer, the early console's                    |
| `0xFA000000`  |          | ACPI's tables                                           |
| `0xFA400000`  | 88 MiB   | the window for memory past the direct map and devices' registers |

RAM past 768 MiB isn't used on i386: it wouldn't fit in the direct map.

x86_64:

| From                    | Size     | What                                          |
|-------------------------|----------|-----------------------------------------------|
| `0x0000000000000000`    | 128 TiB  | user space: programs at `0x400000`, the stack below `0x0000800000000000` |
| `0xFFFF800000000000`    | all RAM  | the direct map, with 2 MiB pages              |
| `0xFFFFC00000000000`    |          | the framebuffer, the early console's, ACPI's tables |
| `0xFFFFC00300000000`    | 64 GiB   | devices' registers                            |
| `0xFFFFD00000000000`    | 1 GiB    | kvmem                                         |
| `0xFFFFE00000000000`    | 16 GiB   | slabs                                         |
| `0xFFFFFFFF80000000`    | 2 GiB    | the kernel                                    |

### The pmap

`pmap_*()` are in [`mm/i386.c`](../../kernel/arch/i386/mm/i386.c) and
[`mm/x86_64.c`](../../kernel/arch/i386/mm/x86_64.c). A `struct pmap` is
the top table's physical address, a reference count, and the CPUs it's
loaded on. Tables are reached through the direct map; a missing table is
allocated when a page is mapped below it (`pte_get()`), and freed with
the address space. `pmap_switch()` loads `CR3`.

`arch_mm_page_fault()` turns the CPU's page fault into
`mm_page_fault()`'s arguments: the address from `CR2`, and the error
code's bits (present, write, user, instruction fetch) into `PF_*` flags.

Shortcuts: AquilaOS doesn't use the no-execute bit, so on x86 a readable
page is also executable (`VM_UX` isn't enforced); the device windows are
handed out and never given back.

## aarch64: page tables

aarch64's tables ([`mm/pmap.c`](../../kernel/arch/aarch64/mm/pmap.c)) have
4 levels of 512 entries, with 4 KiB pages and 48-bit addresses, as on
x86_64. The address space has two halves, each with its own top table:
user space is `TTBR0_EL1`'s, one table for each pmap, and the kernel's
half is `TTBR1_EL1`'s, one table for all, set at boot and never switched.
The kernel's mappings are then the same in every address space without
copying entries, and `pmap_switch()` only loads `TTBR0_EL1`, flushing the
TLB (there are no ASIDs).

The layout (`ARCH_*` in `kernel/arch/aarch64/include/mm/mm.h`):

| From                    | Size     | What                                          |
|-------------------------|----------|-----------------------------------------------|
| `0x0000000000000000`    | 128 TiB  | user space, the stack below `0x0000800000000000` |
| `0xFFFF000000000000`    | all RAM  | the direct map, with 1 GiB blocks, the kernel in it |
| `0xFFFF800000000000`    | 1 GiB    | kvmem                                         |
| `0xFFFF900000000000`    | 16 GiB   | slabs                                         |
| `0xFFFFA00000000000`    | 64 GiB   | devices' registers, and memory past the direct map |

A fault is an *abort*: `abort()` in [`cpu/trap.c`](../../kernel/arch/aarch64/cpu/trap.c)
takes the address from `FAR_EL1` and the kind of access from `ESR_EL1`
(an instruction abort, a write, a permission fault for a page that is
there) into `PF_*` flags. Unlike x86, aarch64 enforces `VM_UX`: a page
without it has `UXN` set.

## Where it is

| File                                             | Layer   | What                                         |
|--------------------------------------------------|---------|----------------------------------------------|
| `kernel/mm/mm.c`                                 | kernel  | `pages[]`, boot memory, `mm_setup()`         |
| `kernel/mm/buddy.c`                              | kernel  | the buddy allocator                          |
| `kernel/mm/kvmem.c`                              | kernel  | `kmalloc()`, `kfree()`, big objects, guard pages |
| `kernel/mm/slab.c`                               | kernel  | small objects                                |
| `kernel/mm/vmem.c`                               | kernel  | the vmem range allocator                     |
| `kernel/mm/vm_space.c`, `vm_entry.c`, `vmm.c`    | kernel  | address spaces and regions, `fork`           |
| `kernel/mm/vm_anon.c`                            | kernel  | private pages                                |
| `kernel/mm/vm_object.c`, `kernel/fs/vm_object.c` | kernel  | a file's pages                               |
| `kernel/mm/fault.c`                              | kernel  | `mm_page_fault()`                            |
| `kernel/sys/binfmt/`                             | kernel  | the regions of a new program                 |
| `kernel/sys/sys_proc.c`                          | kernel  | `sbrk()`, `mmap()`, `munmap()`               |
| `kernel/include/mm/`                             | kernel  | the structures, and the `pmap` interface     |
| `kernel/arch/i386/include/mm/mm.h`               | x86     | the address space layouts                    |
| `kernel/arch/i386/mm/i386.c`                     | x86     | 32-bit paging                                |
| `kernel/arch/i386/mm/x86_64.c`                   | x86     | 4-level paging                               |
| `kernel/arch/i386/mm/mm.c`                       | x86     | `arch_mm_setup()`                            |
| `kernel/arch/aarch64/include/mm/mm.h`            | aarch64 | the address space layout                     |
| `kernel/arch/aarch64/mm/pmap.c`                  | aarch64 | 4-level paging, `arch_mm_setup()`            |
| `kernel/arch/aarch64/cpu/trap.c`                 | aarch64 | page faults (aborts)                         |

## References

- *The Design and Implementation of the FreeBSD Operating System*,
  McKusick, Neville-Neil and Watson: chapter 6, memory management
  (address spaces, objects, the pmap layer, `malloc(9)` types).
- Charles D. Cranor, *The Design and Implementation of the UVM Virtual
  Memory System*, PhD thesis, Washington University, 1998: amaps and
  anons, which `vm_anon` and `vm_aref` follow.
- Jeff Bonwick, *The Slab Allocator: An Object-Caching Kernel Memory
  Allocator*, USENIX 1994; Jeff Bonwick and Jonathan Adams, *Magazines
  and Vmem*, USENIX 2001.
- Donald Knuth, *The Art of Computer Programming*, Volume 1, 2.5: the
  buddy system.
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3,
  chapter 4: *Paging* (32-bit paging, 4-level paging, page fault error
  codes).
- Arm Architecture Reference Manual for A-profile, D8: *The AArch64
  Virtual Memory System Architecture* (translation tables, `TTBR0_EL1`
  and `TTBR1_EL1`, aborts).
- The OSDev wiki's pages on [Paging](https://wiki.osdev.org/Paging),
  [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)
  and [Copy On Write](https://wiki.osdev.org/Copy_On_Write).
