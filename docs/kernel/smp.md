# SMP and the kernel lock

A machine with several CPUs that share the same memory, and are all
equal, is a *symmetric multiprocessor* (SMP). Each CPU runs its own
thread at the same time as the others, so two programs really run at
once. For the kernel this raises two problems:

- **Two CPUs in the kernel at once** could both change the same
  structure (the ready queue, a file's offset, a page table) and leave
  it broken. On one CPU, AquilaOS avoided this by running the kernel with
  interrupts off (see [Interrupts](interrupts.md)); with several, that
  isn't enough.
- **Each CPU has its own caches of what the kernel set up**: its TLB
  remembers address translations, its FPU a thread's registers. When one
  CPU changes something another may have cached, it has to tell it.

This document describes how the kernel solves both, and then how x86
starts the other CPUs and lets them talk to each other; aarch64 runs on
one CPU so far (see
[The kernel and the machine](architecture.md) for the split).

## The kernel's side

### Each CPU's own

Every CPU has a `struct cpu` (`kernel/include/sys/cpu.h`), in the array
`cpus[]`; `cpus[0]` is the one that booted:

```c
struct cpu {
    struct cpu *self;       /* curcpu() reads it: it comes first */
    int id;                 /* its index in cpus[] */
    struct thread *thread;  /* the running thread (curthread), or NULL */
    int idle;               /* none of the threads runs (kernel_idle) */
    int kicked;             /* woken to run one (sched_cpu_wake) */
    struct arch_cpu arch;
};
```

`curcpu()` returns the running CPU's, however the architecture finds it.
`curthread` and `curproc`, used all over the kernel, are its running
thread and that thread's process: per CPU, not global. Each CPU also has
its own idle stack, where it waits when there's nothing to run.

There are at most 32 CPUs (`CPUS_MAX`): sets of CPUs are 32-bit masks.

### One kernel lock

The classic answers to two CPUs in the kernel are:

- **Fine-grained locks**: a lock for each structure, taken around every
  use of it. Every CPU can be in the kernel at once, working on different
  things. This is what Linux and the BSDs do today, with years of work
  behind it, and many bugs found on the way: deadlocks, a lock forgotten
  in one path.
- **One big lock**: only one CPU at a time runs the kernel; the others
  run user programs, or wait. The kernel stays as it was on one CPU.
  This is how Linux 2.0 (its "big kernel lock") and FreeBSD before 5.0
  first ran on SMP machines.

AquilaOS takes the second: the **kernel lock**
([`sys/lock.c`](../../kernel/sys/lock.c)). A CPU takes it when it enters
the kernel from user mode or from its idle loop (`kernel_enter()`), and
lets it go when it goes back to either (`kernel_leave()`). An exception
in the kernel finds the lock already held by its CPU and does nothing.

The lock belongs to the CPU, not to a thread. A thread that sleeps in
the kernel doesn't let it go: its CPU, still in the kernel, runs the next
thread; and if there's none, its idle loop lets the lock go. A thread
woken later may run on another CPU, which has the lock because it
entered the kernel to run it. "Holding the lock" means "this CPU is in
the kernel".

It's a *ticket lock*: a CPU takes a number and waits until it's served,
so the CPUs get it in the order they asked. While a CPU waits, it spins
with interrupts off; it still does what the CPU holding the lock asks of
the others (`arch_lock_spin()`: flush its TLB, below), because that CPU
waits for it.

Why one lock: the kernel was written for one CPU, and its central rule,
that what runs in the kernel isn't interrupted, carries over whole. No
structure needs its own lock, and none of the code had to change for
SMP. It costs scalability: programs that compute run on all CPUs at
once, but programs that make many system calls queue for the kernel.
UnixBench's index on x86_64 under KVM went from 2249 on one CPU to 2973
on four, not four times as much. For a kernel meant to be read, that's
the right trade.

### Running threads on several CPUs

There is one ready queue for all the CPUs (see [Processes and threads](processes.md)).
A CPU whose thread sleeps or whose time slice is over takes the next
ready thread, whichever CPU it last ran on: no CPU has its own queue,
and a thread has no preferred CPU.

- **Ticks**: every CPU has its own timer tick, which charges its
  running process its time (`sched_cpu_tick()`), and on which the
  architecture asks for a switch. Only the boot CPU's moves the clock on,
  runs the timeouts and wakes the sleepers (`sched_timer_tick()`).
- **Waking an idle CPU**: when a thread becomes ready
  (`sched_thread_ready()`), an idle CPU is asked to run it now, not at
  its next tick (`sched_cpu_wake()`, `arch_cpu_kick()`). `kicked` marks
  a CPU already asked, so that the next ready thread wakes another one.
  The CPU, woken by an interrupt, takes the lock and runs the thread on
  its way out of the interrupt (`sched_idle_wakeup()`).
- **Device interrupts** all go to the boot CPU. The others only get
  their tick and the messages below.

### Killing a thread that runs elsewhere

When a process exits or execs, its other threads must end
(`proc_threads_end()`, in [`sys/proc.c`](../../kernel/sys/proc.c)). A
thread that is sleeping or ready is freed at once. One that is running
on another CPU can't be: that CPU is in user mode, on the thread's
stack. So the thread is marked `thread->killed`, and its CPU is kicked
(`sched_thread_kick()`). When it enters the kernel, which it does at
once, it finds the mark and the thread frees itself
(`sched_thread_killed()`), from the CPU's idle stack. If what made it
enter was a device interrupt, the interrupt is handled first.

### TLB shootdown

A CPU caches address translations in its *TLB*, and doesn't notice when
the page tables change. When the kernel unmaps a page, or makes it
read-only, the CPU that did it flushes its own TLB, but another CPU
running a thread of the same process could keep using the old
translation, and write to a page that was freed and given to someone
else.

So when a mapping changes, the CPUs that may have it must flush it
before the change is done: a *TLB shootdown*. The pmap does it for every
change (`tlb_shootdown()`), and knows which CPUs to ask: each address
space keeps the set of CPUs it's loaded on (`pmap->cpus`, kept by
`pmap_switch()`). The CPU sends them a message and waits until each has
flushed. The others do it without the kernel lock, which is held by the
CPU waiting for them: from the message's interrupt, or, if they're
spinning for the lock with interrupts off, from the spin.

The kernel's own mappings are in every address space, so every CPU may
have them. But a CPU without the lock only touches memory that was mapped
before it took the lock last and stays mapped (its stacks, its
`struct cpu`, the lock): so they don't need a message. Each change to the
kernel's mappings counts one more *generation* (`kernel_gen`), and a CPU
that takes the lock flushes its TLB if the generation moved since it
last did (`arch_lock_taken()`).

The shootdown itself is the architecture's
([`mm/tlb.c`](../../kernel/arch/i386/mm/tlb.c)): what a TLB is and how
to flush it are x86's. But the rules above are what any architecture
would follow.

### What else is per CPU

Anything a CPU keeps in its registers for a thread has the same
problem. On x86, the FPU's state is switched lazily (a thread's first
FPU instruction after a switch traps, see [Interrupts](interrupts.md));
each CPU remembers whose state its FPU has (`arch.fpu_thread`). A thread
switched out has its state saved first (`x86_fpu_switch_out()`), so it
can go on on any CPU. Any architecture with lazily switched state has to
do the same; aarch64 switches its FP/SIMD registers lazily too, and each
CPU remembers whose they are (`arch.fp_thread`).

## x86: finding the CPU

On x86, `curcpu()` reads the `struct cpu` pointer at `%gs:0`
(`kernel/arch/i386/include/cpu/cpu.h`):

- on i386, `%gs` is loaded with a segment of the CPU's own GDT whose
  base is its `struct cpu`;
- on x86_64, the CPU's `GS_BASE` MSR is its `struct cpu`. User mode has
  its own GS base, swapped in and out by `swapgs` on the way from and to
  user mode.

Each CPU has its own GDT and TSS (a TSS is busy on the CPU that loaded
it, and holds the kernel stack the CPU switches to on an interrupt from
user mode), in `struct arch_cpu`, with its local APIC's ID and its
loaded page tables.

## x86: starting the other CPUs

At reset only one CPU runs, the *bootstrap processor*; the others, the
*application processors* (APs), wait for a message. `x86_smp_init()`
([`cpu/smp.c`](../../kernel/arch/i386/cpu/smp.c)) starts them at boot,
unless `smp=off`, as the Intel manual describes:

1. The CPUs are the local APICs listed in ACPI's MADT (`x86_apic_cpus()`).
2. The *trampoline* ([`cpu/trampoline.S`](../../kernel/arch/i386/cpu/trampoline.S))
   is copied to the page at `0x8000`, below 1 MiB: an AP starts in 16-bit
   real mode and can only reach that far. With it go the boot CPU's
   control registers (`CR0`, `CR4`, `EFER`), the trampoline's page
   tables, and the AP's entry, stack and `struct cpu`.
3. Each AP is sent an **INIT** IPI, then 10 ms later a **startup IPI**
   (SIPI) whose vector is the trampoline's page number, then a second
   SIPI if it didn't answer. It has 100 ms more to say it's there.
4. The trampoline loads a GDT, turns on protected mode, loads the boot
   CPU's `CR4`, the trampoline's tables and on x86_64 `EFER` (long mode),
   turns on paging, and jumps to the kernel's `x86_ap_start()`.
5. `x86_ap_start()` sets up the CPU's GDT, TSS and `%gs`, loads the IDT,
   switches to the kernel's page tables (the trampoline's map the
   trampoline's page and the kernel's half), enables its local APIC,
   says it's there, and idles.

The APs wait idle until the scheduler starts: `arch_sched_init()` kicks
each one, and the first wake IPI starts its timer
(`x86_lapic_timer_cpu_start()`). The PIT times the waits, which is why
`platform_init()` starts the CPUs after it (see [Booting](booting.md)).

Without an APIC (`apic=off`), there's no way to start them: AquilaOS
runs on one CPU.

## aarch64: one CPU

aarch64 runs on the boot CPU only: the other CPUs of the virt machine
are never started. Its `curcpu()` reads `tpidr_el1`, which
`aarch64_init()` sets to `cpus[0]`
(`kernel/arch/aarch64/include/cpu/cpu.h`). The kernel lock is taken and
let go as on x86, but nothing ever waits for it, so the architecture's
side has nothing to do: `arch_cpu_kick()`, `arch_lock_spin()` and
`arch_lock_taken()` are empty
([`sys/smp.c`](../../kernel/arch/aarch64/sys/smp.c)), and
`tlb_shootdown()` flushes this CPU's TLB only
([`mm/pmap.c`](../../kernel/arch/aarch64/mm/pmap.c)).

## x86: messages between CPUs

The local APIC sends the *inter-processor interrupts* (IPIs), to a CPU
by its APIC ID (`x86_lapic_ipi()`):

| Vector  | Sent by              | What the CPU does                                |
|---------|----------------------|--------------------------------------------------|
| `0xF1`  | `tlb_shootdown()`    | flush the range asked (`invlpg`, or reload `CR3` past 32 pages), or switch to the kernel's tables if the address space is going away; without the kernel lock |
| `0xF2`  | `arch_cpu_kick()`    | enter the kernel: run a ready thread, take a signal, end a killed thread; with the kernel lock |

The TLB message is handled in `__x86_irq_handler()` before the lock is
taken: the sender holds it. A request is one at a time (only the CPU
with the lock sends them): its pages, and a mask of the CPUs that haven't
done it yet, which each clears.

## Where it is

| File                                         | Layer  | What                                        |
|----------------------------------------------|--------|---------------------------------------------|
| `kernel/include/sys/cpu.h`                   | kernel | `struct cpu`, `cpus[]`                      |
| `kernel/sys/lock.c`, `kernel/include/sys/lock.h` | kernel | the kernel lock                         |
| `kernel/sys/sched.c`                         | kernel | ticks, waking idle CPUs, killed threads     |
| `kernel/sys/proc.c`                          | kernel | ending a process's threads (`proc_threads_end()`) |
| `kernel/arch/i386/include/cpu/cpu.h`         | x86    | `struct arch_cpu`, `curcpu()`               |
| `kernel/arch/i386/cpu/gdt.c`                 | x86    | each CPU's GDT, TSS and `%gs`               |
| `kernel/arch/i386/cpu/smp.c`                 | x86    | starting the APs, `arch_cpu_kick()`         |
| `kernel/arch/i386/cpu/trampoline.S`          | x86    | from real mode to the kernel                |
| `kernel/arch/i386/mm/tlb.c`                  | x86    | TLB shootdown, `arch_lock_spin()`, `arch_lock_taken()` |
| `kernel/arch/i386/cpu/isr.c`, `platform/misc/irq.c` | x86 | taking the lock on the way in        |
| `kernel/arch/i386/platform/misc/apic.c`      | PC     | the local APIC: IPIs, each CPU's timer      |
| `kernel/arch/aarch64/include/cpu/cpu.h`      | aarch64 | `struct arch_cpu`, `curcpu()`              |
| `kernel/arch/aarch64/sys/smp.c`              | aarch64 | the lock's and the kick's hooks, empty     |

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3:
  *Multiple-Processor Management* (9.4, the INIT-SIPI-SIPI sequence),
  *Advanced Programmable Interrupt Controller* (IPIs) and *Paging*
  (4.10.4, invalidating TLBs).
- *MultiProcessor Specification*, version 1.4, Intel: the original
  description of starting APs.
- Curt Schimmel, *UNIX Systems for Modern Architectures: Symmetric
  Multiprocessing and Caching for Kernel Programmers*, Addison-Wesley,
  1994: master-slave kernels, big locks, fine-grained locking, and TLB
  consistency.
- John M. Mellor-Crummey and Michael L. Scott, *Algorithms for Scalable
  Synchronization on Shared-Memory Multiprocessors*, ACM TOCS, 1991: the
  ticket lock.
- The OSDev wiki's pages on [Symmetric Multiprocessing](https://wiki.osdev.org/Symmetric_Multiprocessing)
  and the [TLB](https://wiki.osdev.org/TLB).
