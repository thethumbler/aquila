# Booting

When a computer is switched on, its firmware finds a boot loader and runs
it. The boot loader loads the kernel into memory, and whatever else the
kernel asks for, and jumps to it. From there the kernel is on its own: it
has to set up the CPU, find out how much memory there is and take it
over, find its devices, mount a root filesystem, and start the first
program, *init*, in user mode. Every other process descends from init.

Booting crosses all three layers (see [The kernel and the machine](architecture.md)):
the platform decides how the kernel is loaded and what it is told, the
architecture brings the CPU to a state where C code can run at the
kernel's addresses, and the kernel does the rest, the same everywhere.
This document starts with what the kernel needs and does, then follows
the PC and x86, and then QEMU's virt machine and aarch64, from the boot
loader's jump to `kmain()`.

## The kernel's side

### What the kernel is given

The architecture hands the kernel a `struct boot`
(`kernel/include/boot/boot.h`), the machine-independent summary of what
the boot loader said:

```c
struct boot {
    char *cmdline;          /* the kernel's command line */
    uintptr_t total_mem;    /* how much memory, in KiB, if there's no map */
    int modules_count;      /* the files loaded with the kernel */
    int mmap_count;
    module_t *modules;      /* each: physical address, size, command line */
    mmap_t *mmap;           /* the memory map: usable or reserved ranges */
    elf_shdr_t *shdr;       /* the kernel's sections and symbols, */
    uint32_t shdr_num;      /* for backtraces */
    elf_shdr_t *symtab;
    size_t symnum;
    elf_shdr_t *strtab;
};
```

`elf_shdr_t` is the kernel's own ELF class: 32-bit on i386, 64-bit on
x86_64 and aarch64. The kernel never sees the boot loader's own
structures: the PC fills `struct boot` from Multiboot's information,
aarch64 from the devicetree. `__kboot` points to it.

### The order of things

Each step needs the ones before it. The architecture runs the first ones,
calling the kernel's functions where the work is generic, and ends by
calling `kmain()`:

| Step                                   | Layer        | x86                               | aarch64                           |
|----------------------------------------|--------------|-----------------------------------|-----------------------------------|
| the CPU at the kernel's addresses      | architecture | the boot code                     | `boot/head.S`                     |
| this CPU's own state (`curcpu()`)      | architecture | `x86_gdt_setup()`                 | `tpidr_el1`                       |
| the kernel lock, taken until init runs | kernel       | `kernel_lock()`                   | `kernel_lock()`                   |
| the early console: `printk()` works    | platform     | `earlycon_init()`                 | `earlycon_init()`                 |
| traps reported, not fatal              | architecture | `x86_idt_setup()`, `x86_isr_setup()` | `vbar_el1`                     |
| `struct boot` filled in                | platform     | `process_multiboot_info()`        | `fdt_boot()`                      |
| physical memory taken over             | kernel       | `mm_setup()`, then `pmap_init()` (architecture) | the same            |
| the kernel's allocator                 | kernel       | `kvmem_setup()`                   | `kvmem_setup()`                   |
| the command line                       | kernel       | `kargs_parse()`                   | `kargs_parse()`                   |
| the console on the screen              | platform     | `earlycon_reinit()`               | `earlycon_reinit()` (nothing to do) |
| interrupt controllers, timers, CPUs    | platform     | `platform_init()`                 | `platform_init()`                 |
| devices, filesystems, the root, init   | kernel       | `kmain()`                         | `kmain()`                         |

On x86 the architecture's part is `x86_cpu_init()`
([`cpu/init.c`](../../kernel/arch/i386/cpu/init.c)), on aarch64
`aarch64_init()` ([`cpu/init.c`](../../kernel/arch/aarch64/cpu/init.c));
both are described below.

### Memory

The memory map says which ranges of physical memory are RAM (*usable*)
and which aren't: the firmware's, the devices', the boot loader's
tables. `mm_setup()` ([`mm/mm.c`](../../kernel/mm/mm.c)) takes it over
before there is any allocator:

1. It finds the end of RAM, and how much there is. An architecture can
   set a limit (`ARCH_RAM_MAX`): i386 uses the first 768 MiB only, what
   fits in its direct map. Without a memory map, it takes `total_mem`
   from 0.
2. It sets aside room for its own structures: `pages[]`, a
   `struct vm_page` for each physical page, and the buddy allocator's
   bitmaps. They go in the first free RAM after the kernel that the boot
   page tables map (`boot_mem_place()`, `ARCH_BOOT_MAPPED`), and are
   handed out from there by `mm_boot_alloc()`.
3. It sets up the *buddy allocator*, the physical page allocator, over
   all of RAM, and marks used what isn't free: the kernel, those
   structures, the memory map's holes and reserved ranges, and the
   modules.
4. It hands over to the architecture (`arch_mm_setup()`), which keeps
   what it needs out of the allocator and builds the kernel's page tables
   (`pmap_init()`).

`kvmem_setup()` then starts the kernel's own allocator, `kmalloc()`.
See [Memory](memory.md).

### The command line

The command line is parsed (`kargs_parse()`,
[`core/kargs.c`](../../kernel/core/kargs.c)) into `key` and `key=value`
arguments, which any part of the kernel looks up with `kargs_get()`. The
arguments read at boot:

| Argument                 | What                                                        |
|--------------------------|-------------------------------------------------------------|
| `root=`                  | the root filesystem is this disk partition (`/dev/hda2`), not the initramfs |
| `rootfstype=`            | the root filesystem's type: `ext2` (the default) or `minix` |
| `ip=`                    | eth0's address, prefix length and gateway (`10.0.2.15/24:10.0.2.2`); see [Networking](networking.md) |
| `ata.nodma`              | ATA disks use PIO, not DMA                                  |
| `kvmem.test`, `kvmem.guard` | check the kernel's allocator at boot                     |
| `earlycon=`              | PC: where the kernel's messages go: `ttyS0`, `vga` or `fb` (default: the framebuffer if there is one, else the serial port) |
| `apic=off`               | PC: the 8259s, not the APIC (and one CPU)                   |
| `i8042.nocheck`          | PC: don't check the keyboard controller's self-test         |
| `i8254.div=`             | PC: the PIT's divisor, when it gives the tick               |
| `smp=off`                | x86: the boot CPU only                                      |
| `clock=`                 | x86: the clock's counter: `tsc`, `hpet` or `pit` (default: the TSC if it is invariant, else the HPET, else the ticks) |

### kmain

`kmain()` ([`core/main.c`](../../kernel/core/main.c)) is the same on every
architecture:

1. With `kvmem.test` or `kvmem.guard` on the command line, it checks the
   kernel's allocator (the `kvmem` test).
2. **The devices and filesystems**: the device layer (`kdev_init()`), the
   VFS (`vfs_init()`), then every *built-in module* (`modules_init()`,
   [`core/module.c`](../../kernel/core/module.c)). A driver or filesystem
   declares its init function with `MODULE_INIT()`, which puts a pointer
   to it in the `.__minit` section; the linker script gathers them between
   `__minit` and `__minit_end`, and `modules_init()` calls them all. The
   terminals, the serial ports, the disks, the network card, the
   filesystems: all register themselves this way.
3. **The root filesystem**:
   - with `root=` on the command line, that disk partition is mounted,
     as ext2 unless `rootfstype=` says otherwise;
   - otherwise the first module is the *initramfs*, a `cpio` archive
     (`make initrd`), mounted as it is (`load_ramdisk()`,
     [`fs/initramfs/initramfs.c`](../../kernel/fs/initramfs/initramfs.c)):
     a read-only filesystem whose files are read from the module's memory,
     where the boot loader left it;
   - with neither, the kernel panics.
4. **`/dev`**: devfs is mounted on it, with `/dev/console`, which init may
   write to at once.
5. **init**, the first process: below.

### The first process

There is no process to `fork()` the first one from, so it's made by hand.
`kmain()` makes an empty process (`proc_new()`), makes it the current one,
switches to its address space, and loads `/init` into it
(`binfmt_load()`): an ELF program, whose segments are mapped as for
`exec`. In the initramfs, `/init` is a link to runit's `/sbin/runit-init`.
The architecture gives its thread a kernel stack and its user-mode
registers, the program's entry point and the top of the user stack
(`arch_proc_init()`); then its initial stack is built, with its
arguments and environment, as `execve()` does (`init_execve()`). init
has one argument, the module's command line, and no environment.

`sched_init_spawn()` ([`sys/sched.c`](../../kernel/sys/sched.c)) then
starts the system:

1. init's working directory is the root, and its umask `022`.
2. The architecture starts the clock and the scheduler's tick, and wakes
   the other CPUs, which start their own ticks (`arch_sched_init()`).
   init then leads a new session.
3. init's thread is spawned (`arch_thread_spawn()`): the CPU moves to
   its kernel stack, the kernel lock is let go, and the CPU drops to user
   mode at init's first instruction.

The boot stack is never used again. From here on, the kernel only runs
when something enters it: a system call, an interrupt, an exception.

## PC: the boot loader

### Multiboot

On the PC, AquilaOS is loaded with the *Multiboot* protocol (version 1),
which GRUB, Limine and QEMU's `-kernel` all speak. A Multiboot kernel is
an ELF file with a *Multiboot header* in its first 8 KiB: a magic number,
flags that say what the kernel wants, and a checksum. AquilaOS asks for:

- its sections page-aligned (`MULTIBOOT_PAGE_ALIGN`);
- the memory map (`MULTIBOOT_MEMORY_INFO`);
- a graphics mode, 1280×800 with 32 bits a pixel, which the boot loader
  may change (`MULTIBOOT_VIDEO_MODE`);
- on x86_64 only, to be loaded by the addresses in the header
  (`MULTIBOOT_AOUT_KLUDGE`): the boot loaders only load 32-bit ELF files,
  so the 64-bit kernel says where its file goes instead, and its linker
  script lays the file out as it is in memory.

The boot loader loads the kernel at 1 MiB (its physical address), loads
the *modules* (the initial RAM disk), and jumps to the kernel's entry,
`_start`, in 32-bit protected mode with paging off, interrupts off, and
flat segments. `%eax` holds Multiboot's magic number, `%ebx` the physical
address of the *Multiboot information*: the kernel's command line, the
memory map, the modules, the framebuffer, and the kernel's ELF section
headers.

`process_multiboot_info()` ([`arch/i386/include/boot/boot.h`](../../kernel/arch/i386/include/boot/boot.h))
turns it into the `struct boot`. The framebuffer, if the boot loader set a
graphics mode, is added to the memory map as reserved, and registered for
the early console and for `/dev/fb0`.

### Limine

The disk images boot with [Limine](https://github.com/limine-bootloader/limine),
which loads the kernel with its `multiboot1` protocol. Its configuration
is in `boot/`:

| File                    | Used by                                                          |
|-------------------------|------------------------------------------------------------------|
| `boot/limine.conf`      | the CD image (`make iso`): the kernel and `initrd.img`           |
| `boot/limine-hdd.conf`  | the hard disk (`make hdd`): the root filesystem is `root=/dev/hda2`, no module |
| `boot/limine-live.conf` | the live disk (`make live`, i386 only): the kernel and initrd, gzipped |

Each has an entry with the console on the serial port (`earlycon=ttyS0`).
`make run` doesn't need a disk: QEMU loads the kernel and the initrd
itself (`-kernel`, `-initrd`).

### The early console

Until the terminals are up, `printk()` writes through the *early console*
([`earlycon/`](../../kernel/arch/i386/earlycon/earlycon.c)): the first
serial port from the start, then, once the command line is read, the
framebuffer if there is one, else the serial port still; `earlycon=`
chooses one of them, or VGA's text mode.

## x86: where the kernel is

The kernel is linked to run in the *higher half* of the address space,
where it is the same in every process's address space and out of user
space's way, but it is loaded at 1 MiB, before paging is on. The linker
scripts make the difference (`kernel/arch/i386/kernel.i386.ld`,
`kernel.x86_64.ld`):

- The boot code (`kernel/arch/i386/boot/`) is linked at its physical
  address, from 1 MiB: it is what runs before paging is on.
- Everything else is linked at `_VMA` plus its physical address: `_VMA`
  is `0xC0000000` on i386 (the kernel at 3 GiB + 1 MiB), and
  `0xFFFFFFFF80000000` on x86_64 (the kernel in the top 2 GiB, where code
  can use 32-bit addresses: the *kernel code model*). Each section's load
  address (`AT(...)`) is its physical one.

The `VMA()` and `LMA()` macros (`kernel/arch/i386/include/mm/mm.h`) go from
one to the other.

## x86: paging on

The entry, `_start` ([`boot/multiboot.S`](../../kernel/arch/i386/boot/multiboot.S)),
saves `%eax` and `%ebx`, loads its own stack (Multiboot gives it none),
and calls the next step, which turns paging on with *boot page tables*
that map the kernel twice: where it is, so that the code running at its
physical address keeps running, and where it is linked.

### i386

`x86_bootstrap()` ([`boot/init.c`](../../kernel/arch/i386/boot/init.c)),
in C, at its physical address, maps the first 768 MiB of memory (the
whole *direct map*) at 0 and at `0xC0000000` with 4 KiB pages, from a
page directory and tables in the boot code's own `.bss`. It turns paging
on, and write protection in the kernel too (`CR0.WP`: copy on write
depends on it), moves the stack pointer to its higher-half address
(`early_init_fix_stack`, in [`boot/sys.S`](../../kernel/arch/i386/boot/sys.S)),
and calls `x86_cpu_init()`, now at its kernel address.

### x86_64

On x86_64 the CPU must also be brought from 32-bit protected mode to
*long mode*, which needs paging. `x86_64_bootstrap`
([`boot/x86_64_bootstrap.S`](../../kernel/arch/i386/boot/x86_64_bootstrap.S)),
32-bit code, builds four-level page tables in the kernel's `.bss`: a page
directory of 2 MiB pages mapping the first GiB, reached three times:

| Where                          | Why                                                |
|--------------------------------|----------------------------------------------------|
| 0                              | the code that turns paging on runs there           |
| `0xFFFF800000000000`           | the direct map                                     |
| `0xFFFFFFFF80000000` (−2 GiB)  | the kernel                                         |

Then, as the Intel manual says: PAE on (`CR4.PAE`, and global pages),
`CR3` loaded, long mode enabled in the `EFER` MSR, paging and write
protection on in `CR0`. The CPU is then in long mode's *compatibility
mode*; a far jump through a small GDT with a 64-bit code segment brings it
to 64-bit mode. A last jump goes to `x86_64_start`, at the kernel's
address, which loads a 64-bit stack and calls `x86_cpu_init()`.

From there both run the same C code.

## x86: x86_cpu_init

`x86_cpu_init()` ([`cpu/init.c`](../../kernel/arch/i386/cpu/init.c)) runs
the steps of the table above. The x86-specific ones:

- **The boot CPU's `struct cpu`** (`cpus[0]`), its GDT and TSS
  (`x86_gdt_setup()`, [`cpu/gdt.c`](../../kernel/arch/i386/cpu/gdt.c)).
  The GDT has the kernel's and user mode's segments; `%gs` is made to
  point at the CPU's `struct cpu`, so `curcpu()` and `curthread` work from
  here on (see [Interrupts](interrupts.md)).
- **The IDT** and the exception handlers, so that a fault from here on is
  reported, not a triple fault and a reset; the FPU (`x86_fpu_setup()`).
- **The kernel's page tables** (`pmap_init()`, called by
  `arch_mm_setup()`): the boot tables become the kernel's. On x86_64 the
  direct map is extended to all of RAM (with 2 MiB pages); on both, the
  kernel's half gets all its top-level entries now, so that every address
  space made later shares them; and the mapping at 0 is removed, since
  user space goes there. The SMP trampoline's pages (`0x8000` to
  `0xBFFF`: its code and its page tables) are kept out of the allocator.
- **The platform** (`platform_init()`,
  [`platform/pc/init.c`](../../kernel/arch/i386/platform/pc/init.c)):
  PCI's host bridge (configuration mechanism #1, at its ports), the
  interrupt controllers (the APIC, or the 8259s), the PIT, the CMOS
  clock, and the other CPUs. The PC's other devices, the serial ports and
  the keyboard controller, are in its table of devices
  ([`platform/pc/devices.c`](../../kernel/arch/i386/platform/pc/devices.c)),
  which their drivers read.

### x86: the other CPUs

With the APIC, `platform_init()` starts the other CPUs, the *application
processors*, with `x86_smp_init()` ([`cpu/smp.c`](../../kernel/arch/i386/cpu/smp.c)),
unless `smp=off`. Their local APICs are listed in ACPI's MADT. Each one
is sent an INIT IPI and a startup IPI; it starts in 16-bit real mode at
the *trampoline*, a page below 1 MiB (`0x8000`, `cpu/trampoline.S`),
which brings it through protected mode (and long mode) to the kernel's
`x86_ap_start()`, on its own stack and with its own GDT, TSS and
`struct cpu`. It says it's there and waits, idle, until the scheduler
starts. See [SMP and the kernel lock](smp.md).

### x86: into user mode

`arch_thread_spawn()` ([`sys/thread.c`](../../kernel/arch/i386/sys/thread.c))
loads init's page tables, sets the TSS's stack pointer to the thread's
kernel stack (where the CPU will switch on the next interrupt from user
mode), and jumps there; `x86_jump_user()` then `iret`s into user mode,
with the user code and stack segments.

## aarch64: QEMU's virt machine

### The boot loader

On aarch64 the kernel runs on QEMU's `virt` machine, loaded by QEMU's
`-kernel` (`make run` with `CONFIG=aarch64-virt`) as a Linux arm64
*Image*: the kernel's memory image, flat (`kernel-*.img`), with Linux's
Image header at its start. QEMU loads it at its physical address, RAM's
start plus the header's `text_offset` (`0x40080000`), loads the
initrd, and jumps to it with the MMU off and the physical address of
the *devicetree* in `x0`.

`fdt_boot()` ([`platform/virt/fdt.c`](../../kernel/arch/aarch64/platform/virt/fdt.c))
turns the devicetree into the `struct boot`: the RAM (`/memory`'s `reg`),
the initrd (`/chosen`'s `linux,initrd-start` and `linux,initrd-end`, the
one module) and the command line (`/chosen`'s `bootargs`). The early
console is the PL011 UART, from the start; `earlycon=` isn't read.

### Where the kernel is, and the MMU on

The kernel is linked at `_VMA` plus its physical address, with `_VMA`
`0xFFFF000000000000`, the start of the direct map: the kernel is where
the direct map has its memory (`kernel/arch/aarch64/kernel.aarch64.ld`).

The entry, `_start` ([`boot/head.S`](../../kernel/arch/aarch64/boot/head.S)),
drops from EL2 to EL1 if QEMU started it at EL2, clears the `.bss`, and
builds boot page tables that map the first 4 GiB of physical memory with
1 GiB blocks twice: at the same addresses (`TTBR0_EL1`, where this code
runs) and in the direct map (`TTBR1_EL1`). The first GiB, the virt
machine's devices, is device memory. It turns the MMU on, jumps to its
kernel address, loads its stack, and calls `aarch64_init()`.

### aarch64_init

`aarch64_init()` ([`cpu/init.c`](../../kernel/arch/aarch64/cpu/init.c))
runs the steps of the table above:

- **The boot CPU's `struct cpu`** (`cpus[0]`), whose address goes in
  `tpidr_el1`, where `curcpu()` reads it.
- **The exception vectors** (`vbar_el1`), and user mode's use of the
  FP/SIMD registers set to trap (`cpacr_el1`), as x86's FPU does.
- **The kernel's page tables** (`pmap_init()`, called by
  `arch_mm_setup()`,
  [`mm/pmap.c`](../../kernel/arch/aarch64/mm/pmap.c)): the boot tables'
  kernel half becomes the kernel's, with the RAM past 4 GiB added to the
  direct map, and the identity mapping goes.
- **The platform** (`platform_init()`,
  [`platform/virt/init.c`](../../kernel/arch/aarch64/platform/virt/init.c)):
  the GIC, and the PCI Express host bridge (ECAM), whose BARs the kernel
  assigns itself, since no firmware ran. The PL011 is in the platform's
  table of devices
  ([`platform/virt/devices.c`](../../kernel/arch/aarch64/platform/virt/devices.c)).

aarch64 runs on one CPU. `arch_thread_spawn()`
([`sys/thread.c`](../../kernel/arch/aarch64/sys/thread.c)) loads init's
page tables and moves to its kernel stack; the kernel lock is let go, and
`aarch64_jump_user()` `eret`s into user mode.

## A boot

What QEMU shows (x86_64, run as `make run` does but with two CPUs,
`-smp 2`; shortened):

```
x86: Welcome to AquilaOS!
x86: installing IDT
x86: installing ISRs
x86: processing multiboot info block at 0x0000000000009500
kernel: 255 MiB of memory, up to 0x0000000010000000
buddy: Setting up buddy allocator (total memory 0x0000000010000000)
buddy: set unusable: 0x0000000000100000-0x0000000000254FFF
...
buddy: set unusable: 0x0000000000008000-0x000000000000BFFF
x86_64: 256 MiB in the direct map
kernel: cmdline: build/x86_64/kernel/kernel-0.0.1.x86_64 earlycon=ttyS0
pci: configuration mechanism #1 at port 0x0000000000000CF8
i8259: initializing [Master: 0x0000000000000020 (pio), Slave: 0x00000000000000A0 (pio)]
apic: local APIC 0 at 0x00000000FEE00000, 1 I/O APIC(s), the first at 0x00000000FEC00000 (GSI 0, 24 pins)
apic: IRQ 0 is GSI 2
...
x86: interrupts through the apic
i8254: Initializing [0x0000000000000040 (pio)]
cmos: initializing [0x0000000000000070 (pio)]
smp: 2 CPUs
kdev: initializing
kernel: loading builtin modules
...
e1000: 8086:100E at 0x00000000FEBC0000, irq 11, link up
kernel: loading ramdisk
kernel: loading init process
hpet: at 0x00000000FED00000, 10000000 fs a count
clock: hpet, 10000000 fs a count
apic: the timer every 2000000 ns (125046 counts, divided by 16)
AquilaOS version v0.0.2
login:
```

The messages starting `x86:`, `x86_64:`, `i8259:`, `apic:`, `i8254:`,
`cmos:`, `smp:`, `hpet:` and `clock:` are the architecture's and
platform's; `kernel:`, `buddy:`, `pci:` and `kdev:` are the kernel's, and
`e1000:` a driver's. The first `buddy: set unusable` is the kernel
itself, from 1 MiB; the last, the SMP trampoline's pages. The command
line QEMU passes starts with the kernel's file name, which the kernel
ignores. `make run` itself boots one CPU.

## Where it is

| File                                            | Layer    | What                                    |
|-------------------------------------------------|----------|-----------------------------------------|
| `kernel/include/boot/boot.h`                    | kernel   | `struct boot`                           |
| `kernel/mm/mm.c`                                | kernel   | `mm_setup()`: the memory map, the boot memory |
| `kernel/core/kargs.c`                           | kernel   | the command line                        |
| `kernel/core/module.c`                          | kernel   | the built-in modules                    |
| `kernel/core/main.c`                            | kernel   | `kmain()`: the root filesystem, init    |
| `kernel/sys/sched.c`                            | kernel   | `sched_init_spawn()`                    |
| `boot/limine*.conf`                             | PC       | the boot loader's configurations        |
| `kernel/arch/i386/boot/multiboot.S`             | PC       | the Multiboot header, the entry         |
| `kernel/arch/i386/include/boot/boot.h`          | PC       | the Multiboot information to `struct boot` |
| `kernel/arch/i386/earlycon/`                    | PC       | the early console: serial, VGA, framebuffer |
| `kernel/arch/i386/platform/pc/init.c`           | PC       | `platform_init()`                       |
| `kernel/arch/i386/kernel.i386.ld`, `kernel.x86_64.ld` | x86 | where the kernel is linked and loaded |
| `kernel/arch/i386/boot/init.c`, `sys.S`         | x86      | i386: paging on                         |
| `kernel/arch/i386/boot/x86_64_bootstrap.S`      | x86      | x86_64: paging and long mode on         |
| `kernel/arch/i386/cpu/init.c`                   | x86      | `x86_cpu_init()`: the order of it all   |
| `kernel/arch/i386/cpu/gdt.c`                    | x86      | the GDT and the TSS                     |
| `kernel/arch/i386/mm/i386.c`, `x86_64.c`        | x86      | `pmap_init()`: the kernel's page tables |
| `kernel/arch/i386/cpu/smp.c`, `trampoline.S`    | x86      | the other CPUs                          |
| `kernel/arch/i386/sys/thread.c`                 | x86      | into user mode                          |
| `kernel/arch/aarch64/platform/virt/fdt.c`       | virt     | the devicetree to `struct boot`         |
| `kernel/arch/aarch64/platform/virt/init.c`      | virt     | `platform_init()`                       |
| `kernel/arch/aarch64/earlycon/earlycon.c`       | virt     | the early console: the PL011            |
| `kernel/arch/aarch64/kernel.aarch64.ld`         | aarch64  | where the kernel is linked and loaded   |
| `kernel/arch/aarch64/boot/head.S`               | aarch64  | the Image header, the entry, the MMU on |
| `kernel/arch/aarch64/cpu/init.c`                | aarch64  | `aarch64_init()`: the order of it all   |
| `kernel/arch/aarch64/mm/pmap.c`                 | aarch64  | `pmap_init()`: the kernel's page tables |
| `kernel/arch/aarch64/sys/thread.c`              | aarch64  | into user mode                          |

## References

- *The Multiboot Specification*, version 0.6.96, GNU: the header, the
  machine state at the entry, the information structure and the memory
  map.
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3:
  *Paging* (32-bit and 4-level paging), *Mode Switching* (9.8, initializing
  IA-32e mode) and *Multiple-Processor Initialization* (9.4, INIT and
  startup IPIs).
- *System V Application Binary Interface, AMD64 Architecture Processor
  Supplement*: the kernel code model (3.5.1).
- Linux's *Booting AArch64 Linux* (`Documentation/arch/arm64/booting.rst`):
  the Image header, and the machine state at the entry.
- *Devicetree Specification*, release 0.4, chapter 5: the flattened
  devicetree.
- Limine's [`CONFIG.md`](https://github.com/limine-bootloader/limine/blob/trunk/CONFIG.md):
  its configuration file.
- The OSDev wiki's pages on [Multiboot](https://wiki.osdev.org/Multiboot),
  [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) and
  [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode).
