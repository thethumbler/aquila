# The AquilaOS kernel

These documents explain how the AquilaOS kernel works: what each part of
it does, how, and why it is done that way. They are written for people
who want to understand operating systems by reading one, and especially
for those writing their own.

AquilaOS is meant to be *boring*. It has no new ideas: it does what UNIX
kernels have done for decades, the plain way, in code small enough to
read in full. So the documents describe the textbook design of each
subsystem, then how AquilaOS implements it, with pointers into the code
and to the specifications it follows. Where AquilaOS takes a shortcut,
they say so.

There are two kinds of documentation:

- **These documents**, one per subsystem, to read in order or on their
  own. Each says what problem the subsystem solves, how it's solved in
  general, how AquilaOS solves it, where the code is, and where to read
  more.
- **The code reference**, generated from the comments in the sources: what
  each function, structure and file is for. `make docs` builds it, with
  these documents, into `build/docs/kernel/api/` (it needs Doxygen);
  `make docs-serve` serves them on <http://localhost:8000/>.

## The kernel at a glance

AquilaOS's kernel is a monolithic UNIX-like kernel. It runs on x86 PCs,
32-bit (i386) and 64-bit (x86_64), one CPU or several, and on aarch64
(QEMU's virt machine, one CPU). These documents describe the x86 PC as
the machine; [The kernel and the machine](architecture.md) says what
depends on it. On the PC it boots from a
Multiboot boot loader (Limine), with an initial RAM disk (initramfs) as
its first root, or with its root on an ext2 disk. User programs are ELF
binaries built against newlib.

### Three layers

The kernel is written so that what is *kernel design* and what is
*the machine* stay apart:

| Layer                | What                                                         | Where                                  |
|----------------------|--------------------------------------------------------------|----------------------------------------|
| **the kernel**       | processes, memory, files, devices, networking: the design, the same on any machine | `kernel/core`, `mm`, `sys`, `fs`, `dev`, `net` |
| **the architecture** | the CPU: its modes, page tables, traps, context switch (x86, aarch64) | `kernel/arch/i386/` (both i386 and x86_64), `kernel/arch/aarch64/` |
| **the platform**     | the machine around the CPU: boot loader, interrupt controllers, timers, firmware tables (the PC, QEMU's virt machine) | `kernel/arch/i386/platform/`, `kernel/arch/aarch64/platform/` |

The kernel calls the architecture through the `arch_*` and `pmap_*`
functions, and the architecture calls back into the kernel when the
machine does something. [The kernel and the machine](architecture.md)
explains where the line is.

The documents follow the same split. Each one describes the subsystem's
design first, the same on every machine. Sections about one architecture
or platform say so in their title: **"x86: ..."** for the CPU, **"PC:
..."** for the machine around it. If you're here to learn how a kernel
works, the untitled sections are the ones that carry over to any kernel;
the x86 and PC ones are how one machine fills them in.

### The sources

The sources are in `kernel/`:

| Directory      | Layer        | What                                                                |
|----------------|--------------|---------------------------------------------------------------------|
| `core/`        | kernel       | the kernel's entry (`kmain`), its command line, `printk`, the clock |
| `mm/`          | kernel       | memory: the physical page allocator (buddy), the kernel's allocator (vmem, slabs, `kmalloc`), address spaces, page faults |
| `sys/`         | kernel       | processes and threads, the scheduler, `fork` and `exec`, signals, the kernel lock, system calls |
| `fs/`          | kernel       | the virtual filesystem (vnodes, path lookup, files) and the filesystems: initramfs, tmpfs, devfs, devpts, procfs, ext2, minix, pipes |
| `dev/`         | kernel       | devices: the device layer (`kdev`), terminals (virtual terminals, serial ports: 8250, PL011, pseudo-terminals), the memory devices (`null`, `zero`, `random`, `kmsg`), the RAM disk, ATA disks and the block cache, the framebuffer, the i8042 keyboard and mouse controller, PCI, the e1000 network card, the platform's device table (`platform.c`) |
| `net/`         | kernel       | sockets, and the network stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP; Unix sockets |
| `include/`     | kernel       | the kernel's headers, and the interface to the architecture (`core/arch.h`, `mm/pmap.h`) |
| `arch/i386/`   | architecture | x86: booting (`boot/`), the CPU (`cpu/`: GDT, IDT, traps, the FPU, SMP), the early console (`earlycon/`), paging (`mm/`), threads, system calls and signal frames (`sys/`), its headers (`include/`) |
| `arch/i386/platform/` | platform | the PC: interrupt controllers (8259, APIC), timers (PIT, HPET), ACPI, CMOS (`misc/`), and their setup, the table of its devices and rebooting (`pc/`) |
| `arch/aarch64/` | architecture | aarch64: booting (`boot/`), the CPU (`cpu/`: exception vectors, traps, FP/SIMD), the early console (`earlycon/`), paging (`mm/`), threads, system calls and signal frames (`sys/`), its headers (`include/`) |
| `arch/aarch64/platform/` | platform | QEMU's virt machine (`virt/`): the GIC, the generic timer, the PL031 clock, PSCI, the devicetree, the table of its devices |

The system call interface, shared with the C library, is in the
repository's top-level `include/uapi/`.

## The documents

| Document                                   | About                                                     |
|--------------------------------------------|-----------------------------------------------------------|
| [The kernel and the machine](architecture.md) | the three layers, the `arch_*` interface, where the line is |
| [Booting](booting.md)                      | what the kernel is given, the order of its setup, the first process; Multiboot, paging and long mode on |
| [Interrupts](interrupts.md)                | system calls, faults, device interrupts and the tick, to the kernel; the IDT, the 8259, the APIC, MSI |
| [Memory](memory.md)                        | physical pages, the kernel's allocator, address spaces, page faults; x86's page tables |
| [Processes and threads](processes.md)      | processes, threads, the scheduler, sleeping and waking, `fork`, `exec`, `exit` |
| [SMP and the kernel lock](smp.md)          | several CPUs, the kernel lock, TLB shootdown; starting the other CPUs |
| [System calls](syscalls.md)                | the table, arguments and results, user memory, restarts   |
| [Signals](signals.md)                      | sending, blocking, delivering, handlers, `sigreturn`      |
| [The virtual filesystem](vfs.md)           | vnodes, path lookup, mounts, open files, the caches       |
| [Filesystems](filesystems.md)              | initramfs, tmpfs, devfs, devpts, procfs, ext2, minix, pipes |
| [Devices and terminals](devices.md)        | the device layer, disks, terminals and the line discipline, the console, PCI |
| [Networking](networking.md)                | sockets, the network interfaces, Ethernet, ARP, IPv4, UDP, TCP, Unix sockets |
| [Building and testing](building.md)        | the build, the configurations, the images, the tests      |

## Reading the code

A few conventions help:

- **Comments say what and why**, not how: what a function does and
  returns, what a structure holds, why something is done the way it is. A
  function's comment is right above it; the code reference is built from
  those. They are written in plain sentences, with a subject and a verb,
  and names in backticks (`` `size` ``). A function other files call
  (not `static`) also has `@param` for each argument and `@return` for
  its result, which Doxygen checks against the code (`make docs` warns
  about a parameter that doesn't exist):

  ```c
  /**
   * Allocates a block of physical memory from a zone, rounded up to a power
   * of two pages.
   *
   * @param zone  the zone to take it from (`BUDDY_ZONE_DMA` or
   *              `BUDDY_ZONE_NORMAL`)
   * @param _sz   the size wanted, in bytes, at most `BUDDY_MAX_BS`
   * @return the block's physical address, or `(paddr_t) -1` if the zone has
   *         no free block that large
   */
  paddr_t buddy_alloc(int zone, size_t _sz)
  ```

  A function that each architecture implements (`arch_*()`,
  `pmap_*()`, `irq_install()`) is documented once, with its tags, where
  the kernel declares it (`kernel/include/`): that's the interface every
  architecture follows. Each architecture's definition says only what is
  particular to it.

  Doxygen ends a comment's first sentence (its summary) at a `.`, `?` or
  `!` followed by a space, even inside backticks: names such as `` `.` ``
  or `` `CSI ? n h` `` go in a later sentence.
- **Errors are negative `errno` values**: a function returns 0 or a
  positive value on success, `-EINVAL`, `-ENOMEM`, ... on failure. System
  calls hand them back to user space as they are. The exceptions return
  a value, or -1 for "none" where there is no error to tell: a free
  descriptor or process ID (`proc_fd_get()`, `proc_pid_alloc()`), a
  device's IRQ (`pci_irq()`), whether a kernel argument was given
  (`kargs_get()`).
- **The architecture's side** of a generic interface is called `arch_*`
  (`arch_thread_switch`, `arch_syscall_return`) and lives in
  `kernel/arch/<arch>/`; x86-specific functions are called `x86_*`,
  aarch64-specific ones `aarch64_*`.
- **One kernel lock**: the kernel runs with interrupts off, on one CPU at
  a time (see [Interrupts](interrupts.md) and
  [SMP and the kernel lock](smp.md)); a structure shared by several CPUs
  needs no lock of its own.
