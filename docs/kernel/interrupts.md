# Interrupts

An interrupt makes the CPU stop what it is running and jump to a handler,
then return to it. Devices use interrupts to say "something happened" (a
key was pressed, a packet arrived, a disk request finished), the CPU uses
them to report faults (a page that isn't mapped, a division by zero), the
timer uses one to give the scheduler a chance to switch threads, and
programs use one to call the kernel.

How an interrupt reaches the kernel is the machine's business: every CPU
does it its own way, and every machine has its own interrupt controllers.
What the kernel does with it is not. This document starts with the
kernel's side, the same on every machine, then describes how x86 and the
PC, and aarch64 and QEMU's virt machine, bring interrupts to it (see
[The kernel and the machine](architecture.md) for the split).

## The kernel's side

### Four kinds of events

To the kernel, an interrupt is one of four things, each with its own
entry into the kernel:

| Event                         | Comes from                         | The kernel's entry                       |
|-------------------------------|------------------------------------|------------------------------------------|
| a **system call**             | the running program, on purpose    | its handler in `syscall_table[]` (see [System calls](syscalls.md)) |
| a **fault**                   | the running program, by accident   | a page fault: `mm_page_fault()`; anything else: a signal |
| a **device interrupt**        | a device                           | the handlers its driver installed        |
| the **tick**                  | the timer                          | `sched_timer_tick()`, `sched_cpu_tick()` |

The architecture catches each one, saves what was running, works out
which it is, and calls the kernel. The kernel never sees vectors, error
codes or controllers: it gets "a write to this address from user mode
faulted", or "IRQ 11 came".

### The kernel runs with interrupts off

The central decision: **the kernel runs with interrupts disabled**. When
an interrupt or a system call enters the kernel, nothing else interrupts
it until it returns to user mode or goes to sleep. The only place
interrupts are allowed in the kernel is the idle loop, which waits for
the next one.

This is what keeps the kernel simple. A system call never finds a data
structure half-changed by an interrupt handler, so the kernel needs no
locks against its own interrupts, and code is never preempted in the
middle. The price is latency: an interrupt that comes while the kernel
runs waits until the kernel is done. AquilaOS's system calls are short or
sleep, so it waits little. Big kernels (Linux, the BSDs) made the
opposite choice long ago, and pay for it with locks everywhere.

With several CPUs, the same promise is kept by the **kernel lock**: one
CPU at a time runs in the kernel. An interrupt from user mode or from
the idle loop takes the lock on its way in (`kernel_enter()`) and lets
it go on its way out. Device interrupts all go to the boot CPU. See
[SMP and the kernel lock](smp.md).

### Device interrupts

Devices raise numbered *IRQs*. A driver installs a handler for its IRQ,
`irq_install(irq, fn, arg)` ([`dev/irq.h`](../../kernel/include/dev/irq.h),
each architecture's): `fn(arg)` runs when it comes, `arg` saying which
device (two ATA channels, two serial ports, one handler). Several
handlers can share an IRQ (PCI devices share lines), so every handler
checks its device and does nothing if the interrupt wasn't its. The IRQ's
number is the platform's; a driver gets it from the platform's table of
devices, its bus, or an MSI it was given.

A handler does as little as it can: it takes the data off the device (a
received packet, a key), acknowledges it, and wakes the threads waiting
for it (`waitq_wakeup()`, which makes them ready with
`sched_thread_wake()`). The work that takes time (TCP's
processing, a filesystem's) happens in those threads, with interrupts
off as anywhere else in the kernel, but no longer in the handler.

### Switching threads

An interrupt is where threads switch without asking to. After the
handlers ran:

- if the **tick** asked for a switch (the running thread's time slice is
  over), the running thread goes back to the ready queue and the next one
  runs (`schedule()`);
- if the interrupt came while the CPU was **idle** and made a thread
  ready (a disk request finished), that thread runs now, not at the next
  tick (`sched_idle_wakeup()`);
- if an **urgent** thread was woken (the network's, by a packet), it runs
  before the interrupted user thread goes on.

On the way back to user mode, the signals that came meanwhile are
delivered (`signal_take()`, see [Signals](signals.md)).

### The tick

The scheduler's tick is an interrupt every 2 ms, on every CPU
(`arch_sched_init()` sets it up). On each one the tick charges the
running process its time (`sched_cpu_tick()`), and the architecture asks
for a switch, which happens once the interrupt is acknowledged: a time
slice is one tick. The boot CPU's also moves the clock on (the
architecture's), runs the timeouts (callouts), wakes the threads whose
sleep or wait is over, and sends `SIGALRM` to the processes whose timer
expired (`sched_timer_tick()`).

### Messages between CPUs

With several CPUs, one sometimes has to make another do something now:
flush its TLB after page tables changed, or enter the kernel because a
thread became ready for it or one of its threads was killed. The kernel
asks with `tlb_shootdown()` and `arch_cpu_kick()`; the architecture
sends an *inter-processor interrupt* (IPI).

### Faults

A **page fault** goes to the memory manager, `mm_page_fault()`, which maps
the page if the address is part of the process's memory (demand paging,
copy on write; see [Memory](memory.md)). If it isn't:

- from user mode, the process gets `SIGSEGV`;
- from the kernel copying to or from user memory (`copyin()`,
  `copyout()`), the copy stops and returns `EFAULT`;
- anywhere else, it's a bug in the kernel, which panics.

Any other fault from user mode is a signal to the process (a division by
zero is `SIGFPE`, an invalid instruction `SIGILL`); from the kernel, a
panic.

## x86: vectors and the IDT

On x86 every interrupt has a number, its *vector*, from 0 to 255. The
first 32 are the CPU's own *exceptions* (14 is a page fault, 13 a general
protection fault, and so on); the rest are free for the system to use.
The *Interrupt Descriptor Table* (IDT) has an entry, a *gate*, for each
vector: the address of its handler, the code segment to run it in, and
the privilege level allowed to raise it with an `int` instruction.

AquilaOS uses *interrupt gates* everywhere: the CPU clears the interrupt
flag (`IF`) when it enters through one, which is how the kernel runs with
interrupts off. The system call gate (vector `0x80`) is the only one user
mode may raise itself.

AquilaOS's vectors:

| Vector      | What                                                         |
|-------------|--------------------------------------------------------------|
| 0 – 31      | CPU exceptions                                               |
| 32 – 79     | device interrupts, IRQ 0 to 47 (vector = 32 + IRQ)           |
| `0x80`      | system calls (`int $0x80`)                                   |
| `0xF0`      | the local APIC's timer (the tick)                            |
| `0xF1`      | IPI: flush the TLB                                           |
| `0xF2`      | IPI: wake up and enter the kernel (`arch_cpu_kick()`)        |
| `0xFF`      | the local APIC's spurious interrupt                          |

The IDT is one table, shared by every CPU; it is filled in by
`x86_isr_setup()` (exceptions, the system call) and
`x86_irq_gates_setup()` (the rest), and loaded on each CPU by
`x86_idt_setup()`.

When an interrupt comes, the CPU pushes the interrupted context's
instruction pointer, code segment and flags; if it came from user mode it
first switches to the kernel stack named in the *task state segment*
(TSS) and also pushes the user stack pointer and stack segment. Some
exceptions push an *error code* after those (a page fault's says whether
it was a read or a write, from user or kernel mode). Then it jumps to the
gate's handler. `iret` undoes it.

## x86: from the gate to C

Every vector's gate points to a small stub in
[`cpu/cpu.S`](../../kernel/arch/i386/cpu/cpu.S). The stubs make every
interrupt look the same to C:

1. The stub pushes the error code (a 0 if the CPU pushed none) and the
   vector number, then jumps to the common code: `isr_handler` for
   exceptions and system calls, `irq_stub` for the rest.
2. The common code saves the general registers (`push_context`) and
   prepares the kernel's environment (`kernel_entry`): the direction flag
   is cleared, and on i386 the data segment registers are loaded, `%gs`
   with the CPU's own segment. On x86_64, an interrupt from user mode
   swaps the GS base with `swapgs` first. Either way `%gs:0` then points
   to the running CPU's `struct cpu`, which is what `curcpu()` and
   `curthread` read.
3. It calls the C handler with a pointer to what is now on the stack, a
   `struct x86_regs`: the registers, the vector and error code, and what
   the CPU pushed. The handler can read and change the interrupted
   context through it (a system call's return value goes into its `eax`).
4. On the way back the same steps are undone in reverse, and `iret`
   returns to the interrupted code (or, after a thread switch, to another
   thread's).

The C handlers are `__x86_isr()` for exceptions and system calls
([`cpu/isr.c`](../../kernel/arch/i386/cpu/isr.c)) and `__x86_irq_handler()`
for device interrupts and IPIs
([`platform/misc/irq.c`](../../kernel/arch/i386/platform/misc/irq.c)).

### Exceptions

`__x86_isr()` turns the CPU's exceptions into the kernel's events:

- a **page fault** (14): the address is in `CR2`, what was tried in the
  error code; `arch_mm_page_fault()` turns them into `mm_page_fault()`'s
  arguments. When a copy from or to user memory faults, the faulting
  instruction is looked up in a table of the copy routines' instructions
  that may fault, which says where the routine goes on to return the
  error (`uaccess_fixup()`, over the table `x86_uaccess_fixups` in
  [`cpu/uaccess.S`](../../kernel/arch/i386/cpu/uaccess.S));
- **device not available** (7) is how the FPU is switched lazily: a
  thread's first FPU instruction after a switch traps, and the FPU is
  given its state then (`x86_fpu_trap()`);
- **`int $0x80`** is a system call (`arch_syscall()`);
- any other exception: a signal from user mode (division by zero:
  `SIGFPE`, invalid opcode: `SIGILL`, general protection: `SIGSEGV`), a
  panic from the kernel.

### Dispatching device interrupts

`__x86_irq_handler()` handles every vector from 32 on:

1. The IRQ is the vector minus 32 (the local APIC's timer, vector `0xF0`,
   is an IRQ of its own: `X86_IRQ_TIMER`).
2. Each handler installed for the IRQ runs (`irq_install()`), with its
   argument.
3. The interrupt controller is told it's done (*end of interrupt*, EOI).
   For a level-triggered line this must come after the device was
   serviced, or the line, still asserted, would raise the interrupt again
   at once.
4. Then the switches above happen (the tick's handler asked for one with
   `x86_irq_tick_switch()`: `x86_preempt()`, or `schedule()` if the CPU
   was idle), and on the way back to user mode, the signals
   (`x86_return_to_user()`).

A thread switch happens inside the handler, on the interrupted thread's
kernel stack: the thread's registers are saved there, and the stack is
left as it is until the thread runs again and returns through the rest of
the handler. Because the controller was told it's done before the switch,
other interrupts keep coming meanwhile.

The number of times each IRQ came is kept, and `/proc/interrupts` shows
them with the controller and each IRQ's route (with several CPUs, a last
line, `tlb:`, counts each CPU's TLB flushes for another):

```
controller: apic
timer: 2551 lapic
1: 1 ioapic gsi 1 edge
11: 140 ioapic gsi 11 level
24: 33 msi
```

## PC: interrupt controllers

Devices don't talk to the CPU directly: an *interrupt controller* takes
their interrupt lines and turns them into vectors. PCs have had two kinds,
and AquilaOS supports both behind one interface, `struct x86_irq_chip`:

```c
struct x86_irq_chip {
    const char *name;
    void (*mask)(unsigned irq);     /* stop this IRQ */
    void (*unmask)(unsigned irq);   /* let it through */
    void (*eoi)(unsigned irq);      /* the handler is done with it */
    const char *(*describe)(unsigned irq);
    const char *timer;
};
```

An IRQ is unmasked when its first handler is installed, masked when its
handlers are removed, and acknowledged after its handlers ran. Which
controller is used is chosen at boot (`x86_pc_irq_init()`,
[`platform/pc/init.c`](../../kernel/arch/i386/platform/pc/init.c)).

### The 8259 PIC

The original PC's controller, two 8259s chained together, has 16 lines:
the *ISA IRQs*, 0 to 15 (0 the PIT, 1 the keyboard, 4 the first serial
port, 12 the PS/2 mouse, 14 and 15 the ATA channels). The second 8259 is
connected to the first one's line 2.

At boot the 8259s send their interrupts to vectors 8 to 15 and `0x70` to
`0x77`, which collide with the CPU's exceptions. AquilaOS *remaps* them to
32–47 ([`platform/misc/pic.c`](../../kernel/arch/i386/platform/misc/pic.c)),
so that IRQ *n* is vector 32 + *n*. An EOI is a command to the 8259 that
raised it (both, for the second one's lines).

The 8259 is used when there is no APIC, or when the kernel is booted with
`apic=off`. With the APIC, the 8259s are remapped anyway (a stray
interrupt from one lands on a harmless vector) and then masked.

### The APIC

Every x86 CPU since the Pentium has a *local APIC*, and chipsets have one
or more *I/O APICs*
([`platform/misc/apic.c`](../../kernel/arch/i386/platform/misc/apic.c)).

- The **local APIC** is the CPU's own: it receives interrupts for its CPU,
  has a timer, and sends and receives IPIs. Its registers are
  memory-mapped (at `0xFEE00000` usually). Each CPU's is enabled by
  `x86_lapic_cpu_setup()`; it is acknowledged by writing its EOI
  register.
- An **I/O APIC** has a number of input *pins*, numbered system-wide as
  *global system interrupts* (GSIs), and a *redirection entry* for each:
  which vector to send, to which CPU, whether the line is edge- or
  level-triggered, active high or low, masked or not.

Where the APICs are, and how the ISA IRQs map to pins, come from ACPI's
**MADT** table (signature `APIC`). By default ISA IRQ *n* is GSI *n*, but
the MADT has *interrupt source overrides* for the exceptions: on most
machines the PIT's IRQ 0 is wired to pin 2, and some lines are
level-triggered. `x86_apic_setup()` reads the table, enables the local
APIC, masks every pin, and installs the APIC as the system's
`x86_irq_chip`. From then on IRQ *n* is the pin the MADT says it is
(`apic_route()`), sent to the boot CPU with vector 32 + *n*. A PCI
device's IRQ is its configuration space's *interrupt line*, an ISA IRQ
number, which QEMU's chipsets wire to the matching pins (level-triggered,
as the MADT's overrides say). A machine whose PCI lines are only on the
I/O APIC's pins past 15 would need ACPI's `_PRT` table, which AquilaOS
doesn't read; a device with MSI (below) doesn't need it.

### MSI

A PCI device that supports *message signalled interrupts* doesn't need a
line at all: it raises an interrupt by writing a value (the *data*) to an
address (the *message address*), which the local APIC receives. The
address names the CPU, the data the vector.

MSI is PCI's, so the kernel's side is generic: `pci_msi_enable()`
([`dev/pci/pci.c`](../../kernel/dev/pci/pci.c)) asks the architecture for
an IRQ and its message (`arch_msi_alloc()`), writes them to the device's
MSI capability, and turns the device's line off. On the PC, the IRQs for
messages are allocated above the I/O APICs' pins (`x86_irq_alloc()`),
each with a controller of its own, `msi_chip`, whose EOI is the local
APIC's. The e1000 driver uses MSI when the device has it (QEMU's
`e1000e`) and its line otherwise (`e1000`). With the 8259 there are no
messages: `arch_msi_alloc()` fails and the driver falls back to the line.

### The timer

The tick comes from (`platform_timer_setup()`, in
[`platform/pc/init.c`](../../kernel/arch/i386/platform/pc/init.c), called
by [`sys/sched.c`](../../kernel/arch/i386/sys/sched.c)):

- with the APIC, the **local APIC's timer**, periodic, on vector `0xF0`,
  one on each CPU. Its rate depends on the CPU's bus clock, which nothing
  tells the kernel: it is *calibrated*, counted against a clock the
  kernel knows (the TSC or the HPET, or the PIT's channel 2) for 20 ms
  (`timer_calibrate()`);
- with the 8259, the **PIT**, the 8254's channel 0, on IRQ 0, and only
  one CPU.

The clock, `arch_rtime_ns()`, is chosen before the tick starts
(`x86_clock_init()`, [`sys/clock.c`](../../kernel/arch/i386/sys/clock.c)):
the TSC if the CPU says it's invariant (calibrated against the HPET, or
the PIT), else the HPET's counter, else the ticks. `clock=tsc`,
`clock=hpet` or `clock=pit` chooses one.

### IPIs

The local APIC sends the IPIs: `0xF1` to flush another CPU's TLB, `0xF2`
for `arch_cpu_kick()` ([`mm/tlb.c`](../../kernel/arch/i386/mm/tlb.c),
[`cpu/smp.c`](../../kernel/arch/i386/cpu/smp.c)). The TLB flush is handled
*without* the kernel lock: the CPU that sent it holds the lock while it
waits for the others. A CPU spinning for the lock with interrupts off does
the flush from its spin loop instead.

A local APIC that never gets its EOI blocks every vector of the same or
lower priority, the CPU's own tick and IPIs included. So a device
interrupt is always acknowledged, even when the thread it interrupted was
killed meanwhile and frees itself on the way out.

## aarch64: exceptions and the GIC

### From the vectors to C

On aarch64 the exceptions have no numbers: `VBAR_EL1` points to a table
of 16 entries ([`cpu/vectors.S`](../../kernel/arch/aarch64/cpu/vectors.S)),
one for each kind (synchronous, IRQ, FIQ, SError) and each place it came
from (the kernel, user mode, and two that AquilaOS doesn't use). IRQs
are masked on entry (`PSTATE.I`), and stay masked in the kernel. Each
entry saves the registers on the kernel stack, a `struct aarch64_regs`,
and calls C ([`cpu/trap.c`](../../kernel/arch/aarch64/cpu/trap.c)):

- `aarch64_sync()`, for a synchronous exception, whose cause is the
  *exception class* in `ESR_EL1`: `svc` is a system call
  (`arch_syscall()`); an instruction or data abort is a page fault, the
  address in `FAR_EL1` (`mm_page_fault()`, or the copy routines' fixups,
  `uaccess_fixup()`, as on x86); a trapped FP/SIMD use gives the thread
  its registers (`aarch64_fpsimd_trap()`); anything else from user mode
  is a signal (`SIGILL`, `SIGBUS` for a misaligned PC or stack,
  `SIGTRAP` for `brk`), from the kernel a panic.
- `aarch64_irq()`, for an interrupt: the handlers, then the switches
  above, as `__x86_irq_handler()` does.

Both take the kernel lock on the way in (`kernel_enter()`). The other
entries (an FIQ, an SError, an exception from aarch32) panic
(`aarch64_bad()`).

### The GIC

QEMU's virt machine has ARM's GICv2
([`platform/virt/gic.c`](../../kernel/arch/aarch64/platform/virt/gic.c)).
Its *distributor* takes the devices' interrupts (SPIs, from 32) and each
CPU's own (PPIs, 16 to 31), and sends them to the CPUs' *CPU interfaces*.
An IRQ is the GIC's interrupt ID; every one goes to the boot CPU, masked
until its first handler is installed. `irq_handle()`
([`platform/virt/irq.c`](../../kernel/arch/aarch64/platform/virt/irq.c))
reads `GICC_IAR` to learn which interrupt came, runs its handlers, and
writes it to `GICC_EOIR`, before any thread switch. PCI devices
interrupt on the host bridge's four lines (`INTA#` to `INTD#`); there is
no MSI.

### The tick and the clock

The tick is the *virtual timer* of ARM's generic timer, each CPU's own,
on PPI 27 ([`platform/virt/timer.c`](../../kernel/arch/aarch64/platform/virt/timer.c)).
Its counter, `CNTVCT_EL0`, runs at `CNTFRQ_EL0` from boot, so it is the
clock too (`arch_rtime_ns()`): nothing needs calibrating. Each tick sets
the next one, every 2 ms.

## Where it is

| File                                         | Layer    | What                                    |
|----------------------------------------------|----------|-----------------------------------------|
| `kernel/sys/sched.c`                         | kernel   | the tick, switching, waking             |
| `kernel/mm/fault.c`                          | kernel   | `mm_page_fault()`                       |
| `kernel/sys/signal.c`                        | kernel   | signals from faults                     |
| `kernel/dev/pci/pci.c`                       | kernel   | MSI on PCI devices                      |
| `kernel/arch/i386/cpu/idt.c`                 | x86      | the IDT and its gates                   |
| `kernel/arch/i386/cpu/cpu.S`                 | x86      | the entry stubs, the context switch     |
| `kernel/arch/i386/cpu/isr.c`                 | x86      | exceptions and the system call          |
| `kernel/arch/i386/cpu/uaccess.S`             | x86      | copies from and to user memory          |
| `kernel/arch/i386/sys/sched.c`               | x86      | the tick's handler                      |
| `kernel/arch/i386/mm/tlb.c`                  | x86      | the TLB flush IPI                       |
| `kernel/arch/i386/platform/misc/irq.c`       | PC       | IRQs: handlers, dispatch, `/proc/interrupts` |
| `kernel/arch/i386/platform/misc/pic.c`       | PC       | the 8259 PIC                            |
| `kernel/arch/i386/platform/misc/apic.c`      | PC       | the local and I/O APICs, MSI, IPIs      |
| `kernel/arch/i386/platform/misc/pit.c`       | PC       | the PIT (8254)                          |
| `kernel/arch/i386/platform/pc/init.c`        | PC       | which controller and timer are used     |
| `kernel/arch/i386/sys/clock.c`               | x86      | the clock: the TSC, the HPET or the ticks |
| `kernel/arch/i386/platform/misc/hpet.c`      | PC       | the HPET                                |
| `kernel/arch/aarch64/cpu/vectors.S`          | aarch64  | the exception vectors                   |
| `kernel/arch/aarch64/cpu/trap.c`             | aarch64  | exceptions, the system call, interrupts |
| `kernel/arch/aarch64/cpu/uaccess.S`          | aarch64  | copies from and to user memory          |
| `kernel/arch/aarch64/sys/sched.c`            | aarch64  | the tick's handler                      |
| `kernel/arch/aarch64/platform/virt/gic.c`    | virt     | the GIC                                 |
| `kernel/arch/aarch64/platform/virt/irq.c`    | virt     | IRQs: handlers, dispatch, `/proc/interrupts` |
| `kernel/arch/aarch64/platform/virt/timer.c`  | virt     | the generic timer: the tick, the clock  |

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3:
  *Interrupt and Exception Handling* (the IDT, gates, what the CPU pushes,
  the exceptions and their error codes) and *Advanced Programmable
  Interrupt Controller (APIC)* (the local APIC, its timer, IPIs, MSI's
  address and data format).
- *82093AA I/O Advanced Programmable Interrupt Controller (IOAPIC)*,
  Intel: the I/O APIC's registers and redirection entries.
- *ACPI Specification*, section 5.2.12: the MADT and its entries.
- *8259A Programmable Interrupt Controller*, Intel: the initialization
  command words used to remap it.
- *PCI Local Bus Specification*, section 6.8: Message Signaled Interrupts.
- *Arm Architecture Reference Manual for A-profile*: exception entry,
  `ESR_EL1`'s exception classes, and the generic timer (chapter D11).
- *ARM Generic Interrupt Controller Architecture Specification*, version
  2.0: the distributor and the CPU interface.
- The OSDev wiki's pages on the [IDT](https://wiki.osdev.org/Interrupt_Descriptor_Table),
  the [8259 PIC](https://wiki.osdev.org/8259_PIC),
  the [APIC](https://wiki.osdev.org/APIC) and the
  [I/O APIC](https://wiki.osdev.org/IOAPIC).
