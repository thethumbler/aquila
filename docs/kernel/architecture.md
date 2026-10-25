# The kernel and the machine

Most of what a kernel does has nothing to do with the CPU it runs on.
Processes, the scheduler's choices, files, path lookup, pipes, sockets,
TCP: these are the same on a PC, a phone or a mainframe. A smaller part
can't be written without knowing the machine: how to switch from one
thread to another, how page tables look, how an interrupt arrives, how the
machine boots. When you're learning, the two are easy to mix up, because
the books and the code show them side by side.

AquilaOS keeps them apart, and so do these documents. This one explains
where the line is and how it's drawn in the code; the others describe
each subsystem's design first, the same on every machine, then how the
machine-specific part does its share, in sections whose titles say which
machine it is.

## Three layers

| Layer            | What it is                                              | Where                                   |
|------------------|---------------------------------------------------------|-----------------------------------------|
| **The kernel**   | the design: processes, memory, files, devices, networking | `kernel/core`, `mm`, `sys`, `fs`, `dev`, `net` |
| **The architecture** | the CPU: its modes, registers, page tables, traps, context switch | `kernel/arch/i386/` (`boot`, `cpu`, `earlycon`, `mm`, `sys`), `kernel/arch/aarch64/` |
| **The platform** | the machine around the CPU: how it boots, its interrupt controllers, timers, firmware tables | `kernel/arch/i386/platform/`, `kernel/arch/aarch64/platform/` |

- **The kernel** is written in plain C against interfaces. It doesn't know
  what a page table entry looks like or how an interrupt is acknowledged.
- **The architecture** is x86, in two flavours built from the same
  directory: i386 (32-bit) and x86_64 (64-bit), with
  `#if ARCH_BITS==32` where they differ (`kernel/arch/x86_64` is a link
  to `i386/`). It's what an instruction set manual describes: privilege
  levels, the IDT, the GDT and TSS, paging, the FPU, `int $0x80`.
- **The platform** is the PC: what isn't in the CPU but every PC has. The
  8259 and APIC interrupt controllers, the PIT and HPET timers, the CMOS
  clock, ACPI's tables, the Multiboot boot loader, and a table of the
  devices no bus finds (the serial ports, the PS/2 controller: see
  "Devices: drivers and the platform" below).
  Another x86 machine could differ here and not in the architecture.

aarch64 is a second implementation of both: the architecture in
`kernel/arch/aarch64/`, and QEMU's virt machine as its platform (see
"What a new architecture needs" below).

Drivers sit in between. A driver in `kernel/dev/` is generic code for a
particular device (an e1000 network card, an ATA disk, an 8250 serial
port), which could be on any machine that has the device; it reaches the
hardware through the architecture's I/O functions (`io_in8()` and the
others, `cpu/io.h`) at the addresses the platform gives it, and through
interrupts.

### Devices: drivers and the platform

A driver hardcodes neither where its device is nor which interrupt it
raises; it's told:

- **Interrupts** go through one interface, `irq_install(irq, fn, arg)`
  and `irq_uninstall()` ([`dev/irq.h`](../../kernel/include/dev/irq.h)),
  which each architecture implements: `fn(arg)` is called when the
  interrupt comes, an IRQ may have several handlers (a shared line), and
  the first unmasks it. An IRQ is the platform's number for it (the ISA's
  or the I/O APIC's on the PC, the GIC's interrupt ID on the virt
  machine); a driver gets it from the platform, its bus or
  `arch_msi_alloc()`, never makes one up.
- **Devices no bus finds** are in the platform's table
  ([`dev/platform.h`](../../kernel/include/dev/platform.h), the PC's in
  `platform/pc/devices.c`): a name (the driver's: `"8250"`, `"i8042"`,
  `"pl011"`), the registers (ports, or physical memory), the interrupts.
  A driver attaches to each entry of its name,
  `platform_device_get(name, i)`, and `platform_device_io()` gives it
  the registers, mapped, for `io_in8()` and the others. Which drivers
  there are is the build's (`DEV_*` in `configs/`); which devices the
  machine has, the platform's.
- **PCI devices** are found on the bus. How its configuration space is
  reached, which IRQs its interrupt pins come to and where its BARs go
  if no firmware put them is the platform's: a `struct pci_host` it gives
  `pci_host_set()` (see PCI in [Devices](devices.md)).

This is Linux's platform devices and the BSDs' hints, cut down to a name
lookup: no matching, no probing order, no hotplug. Linux can also read
the table from the devicetree; AquilaOS's tables are static.

## Where the line is

The kernel and the architecture meet in three ways.

### Functions the architecture provides

Every function the kernel needs from the machine is declared in
`kernel/include/`, mostly in `core/arch.h`, and called `arch_*`, or is
one of the `pmap_*` functions of `mm/pmap.h`. A few are declared next to
what uses them: `arch_copy_user()` in `sys/uaccess.h`, `arch_cpu_kick()`
in `sys/cpu.h`, `arch_lock_spin()` and `arch_lock_taken()` in
`sys/lock.h`, and the interrupts' `irq_install()` and `irq_uninstall()`
in `dev/irq.h`. An architecture implements all of them except
`arch_msi_alloc()`, which is optional (a weak symbol: aarch64 has none):

| What                        | Functions                                                         |
|-----------------------------|-------------------------------------------------------------------|
| processes and threads       | `arch_proc_init()`, `arch_proc_fork()`, `arch_thread_create()`, `arch_kthread_create()`, `arch_thread_kill()`, `arch_exec_start()` |
| running threads             | `arch_thread_spawn()`, `arch_thread_switch()`, `arch_idle()`, `arch_sleep()`, `arch_yield()`, `arch_cur_thread_kill()`, `arch_sched_init()` |
| system calls                | `arch_syscall_return()`: a call's result, into the thread's registers |
| signals                     | `arch_sigcode_map()`, `arch_sigreturn()`: the handler's frame, and back |
| page tables                 | `pmap_create()`, `pmap_switch()`, `pmap_add()`, `pmap_remove()`, `pmap_protect()`, `pmap_page_copy()`, ...; `arch_mm_setup()`, `arch_page_get_mapping()` |
| physical memory and devices | `arch_phys_map()`, `arch_mmio_map()`, `arch_msi_alloc()`          |
| user memory                 | `arch_copy_user()`, `arch_copy_user_str()`: copies that stop at a fault |
| several CPUs                | `arch_cpu_kick()`, `arch_lock_spin()`, `arch_lock_taken()`        |
| time                        | `arch_rtime_ns()` (since boot), `arch_time_get()` (the date), `arch_cycles()` |
| the machine                 | `arch_reboot()`, `arch_halt()`, `arch_poweroff()`, `arch_stack_trace()` |

The names say what the kernel wants, not how: `arch_thread_switch()`
runs another thread, and the kernel doesn't care that on x86 it means
saving registers on the kernel stack, loading `CR3` and setting the TSS's
stack pointer.

### Headers the architecture completes

Some things the kernel needs are types and constants, not functions: how
big a physical address is, where user space ends, what a CPU's
machine-specific state is. These come from headers the architecture
overrides. The include path lists the ABI headers first, then the
platform's headers, then the architecture's, then the kernel's:

```
-I include/uapi
-I kernel/arch/aarch64/platform/virt/include
-I kernel/arch/aarch64/include
-I kernel/include
-I kernel
```

(That's aarch64's. The PC has no directory of platform headers: its
headers are in the architecture's, `core/platform.h` and
`platform/misc.h`.)

So `#include <mm/mm.h>` finds `kernel/arch/<arch>/include/mm/mm.h` first,
which defines what is the architecture's and then includes the kernel's
own with `#include_next <mm/mm.h>`. The headers completed this way:

| Header          | What the architecture adds                                        |
|-----------------|-------------------------------------------------------------------|
| `config.h`      | `ARCH_BITS` (32 or 64), the machine's name for `uname`           |
| `core/system.h` | `PAGE_SIZE`, and how much a page table maps                       |
| `core/arch.h`   | `struct x86_thread` (a thread's registers), `USER_STACK`, `KERN_STACK_SIZE`, interrupts on and off |
| `cpu/cpu.h`     | `struct arch_cpu` (a CPU's GDT, TSS, page tables), `curcpu()`     |
| `cpu/io.h`      | `io_in8()`, `io_out8()` and the others: a device's registers, ports or memory |
| `mm/mm.h`       | `paddr_t`, `vaddr_t`, the kernel's address space layout, `PHYS2VIRT()` |
| `mm/pmap.h`     | `struct pmap`, an address space's page tables                     |
| `boot/boot.h`   | the boot loader's information to `struct boot`                    |
| `core/panic.h`  | `panic()` (replaced whole)                                        |

The generic structures carry a pointer or a member for the architecture's
part and leave it opaque: a thread's `arch` (`struct x86_thread`), a CPU's
`arch` (`struct arch_cpu`), an address space's `pmap`.

### Functions the architecture calls

The other way round, the architecture calls into the kernel when the
machine does something:

| When                            | The kernel's function                                   |
|---------------------------------|---------------------------------------------------------|
| boot is done                    | `kmain(struct boot *)`                                  |
| a system call                   | `syscall_dispatch()`: the handler in `syscall_table[]`, by number |
| a page fault                    | `mm_page_fault()`, with the address and what was tried  |
| the timer's tick                | `sched_timer_tick()` (the boot CPU), `sched_cpu_tick()` (the others) |
| a switch is due                 | `schedule()`                                            |
| on the way back to user mode    | `signal_take()`: a signal to deliver                    |

Everything in between is the architecture's: which vector a page fault
is, where the faulting address is found (`CR2`), what the error code
means. The kernel gets an address and "write, from user mode".

## What a new architecture needs

To run AquilaOS on another CPU, one would write a `kernel/arch/<name>/`
with:

- the headers above;
- an entry from its boot loader that sets up the CPU, turns on paging,
  fills in a `struct boot` and calls `kmain()` (see [Booting](booting.md));
- an early console, for `printk()` before the terminals are up
  (`earlycon/`);
- trap and interrupt entry that ends in the calls in the last table (see
  [Interrupts](interrupts.md));
- the `arch_*` and `pmap_*` functions;
- a platform: a `platform_init()` that the architecture calls before
  `kmain()`, which sets up the interrupt controller and gives the PCI
  host bridge to `pci_host_set()`; `irq_install()` and `irq_uninstall()`
  over that interrupt controller; and the table of the devices no bus
  finds, `platform_devices[]`;
- a linker script and a build configuration (`configs/`).

`kernel/arch/aarch64/` is the second one: the CPU's part, and QEMU's
virt machine as its platform (`platform/virt/`: the GIC, the generic
timer, the PL031 clock, PSCI, the devicetree, and its devices' table,
the PL011 UART).

## Where the line is blurred

The separation is the design, but it isn't finished; these are the places
where x86 still shows through in generic code:

- **ATA's compatibility mode** uses the ISA's ports and IRQs (14, 15)
  when the IDE controller isn't in native PCI mode: they're the PC's,
  in the driver.
- **VGA text mode** (`dev/tty/vt/vga.c`) writes to `0xB8000` through the
  x86 kernel's mapping of low memory, at the CRT controller's ports.
  Those addresses are VGA's own, any VGA card decodes them, but the
  mapping is x86's.
- **The platform's interface** (`platform_init()`, and on x86
  `platform_timer_setup()`) is each architecture's own, declared in its
  headers (`kernel/arch/i386/include/core/platform.h`,
  `kernel/arch/aarch64/platform/virt/include/platform/virt.h`), not the
  kernel's: the architecture calls it, the kernel doesn't.

When reading the code, a name tells you the layer: `arch_*` is the
interface, `x86_*` and `aarch64_*` are the architecture's or platform's
own, anything else
in `kernel/arch/` is too, and everything outside `kernel/arch/` is the
kernel.

## Where it is

| File                                     | What                                         |
|------------------------------------------|----------------------------------------------|
| `kernel/include/core/arch.h`             | the `arch_*` interface                       |
| `kernel/include/mm/pmap.h`               | the page table interface                     |
| `kernel/include/sys/uaccess.h`           | user memory, over `arch_copy_user()`         |
| `kernel/include/sys/cpu.h`               | `struct cpu`, with the architecture's part   |
| `kernel/include/boot/boot.h`             | `struct boot`: what the kernel gets at boot  |
| `kernel/arch/i386/include/`              | x86's headers                                |
| `kernel/arch/i386/{boot,cpu,mm,sys}/`    | x86: boot, CPU, paging, threads and traps    |
| `kernel/arch/i386/earlycon/`             | x86's early console                          |
| `kernel/arch/i386/platform/`             | the PC (`pc/`, `misc/`)                      |
| `kernel/arch/aarch64/`                   | aarch64, and QEMU's virt machine (`platform/virt/`) |
| `kernel/include/dev/irq.h`               | devices' interrupts                          |
| `kernel/include/dev/platform.h`          | the platform's devices, its table            |
| `kernel/Makefile`                        | the include path                             |
| `configs/`                               | the build configurations                     |

## References

- *Lions' Commentary on UNIX 6th Edition*, John Lions: Sixth Edition UNIX
  had the same split, a little assembly (`m40.s`) under portable C.
- *The Design and Implementation of the FreeBSD Operating System*,
  McKusick, Neville-Neil and Watson: the machine-dependent and
  machine-independent parts of a BSD kernel, and the `pmap` interface,
  which AquilaOS's is named after.
- NetBSD's [`pmap(9)`](https://man.netbsd.org/pmap.9) manual page: the
  same interface, documented in full.
