# Processes and threads

A *process* is a running program: an address space with the program in
it, the files it has open, its current directory, its user and group,
what it does with each signal. A *thread* is one flow of execution in a
process: its registers and its stacks. Every process has at least one
thread; threads of a process share everything but their registers and
stacks. The CPU only ever runs threads; the *scheduler* decides which
one, on each CPU, and for how long.

This document covers what a process and a thread are in AquilaOS, how
they're created (`fork`, `exec`, threads), how they end (`exit`, `wait`,
zombies), how the scheduler shares the CPUs between them, and how a
thread sleeps until something happens. The design is the kernel's, the
same on every machine; switching from one thread to another is the
architecture's, described at the end for x86 and aarch64 (see
[The kernel and the machine](architecture.md)).

## The kernel's side

### Processes

A process is a `struct proc` ([`include/sys/proc.h`](../../kernel/include/sys/proc.h)):

| Field                         | What                                                    |
|-------------------------------|---------------------------------------------------------|
| `pid`, `parent`               | its id, and the process that made it                    |
| `pgrp`                        | its process group, and through it its session           |
| `name`                        | the program's path (`ps`, `/proc` show it)              |
| `vm_space`                    | its address space: its mappings, and its page tables (`pmap`) |
| `heap`, `heap_vm`, `stack_vm` | its heap (`sbrk`) and its stack                         |
| `fds`                         | its file descriptors, 64 (`FDS_COUNT`), each an open file |
| `cwd`, `mask`                 | its current directory and its umask                     |
| `uid`, `gid`                  | who it runs as                                          |
| `threads`                     | its threads                                             |
| `sigaction`, `sigmask`, `sig_pending` | its signals (see [Signals](signals.md))         |
| `utime`, `stime`, `cutime`, `cstime` | the CPU time it used, and its children's      |
| `alarm_at`, `alarm_interval`  | its real-time timer (`setitimer`, `alarm`)              |
| `exit`, `running`, `zombie`   | how it ended, and whether it has                        |
| `stopped`, `wait_event`       | job control: stopped by which signal, what `waitpid` has to say |

Every process is on one list, `procs`. A pid is the next free number
after the last one given out, in a bitmap of 4096, wrapping around and
skipping the numbers a process group or a session still uses as its id
(`proc_pid_alloc()`, [`sys/proc.c`](../../kernel/sys/proc.c));
a process is found by its pid by walking the list (`proc_pid_find()`).
Two are special:

- **process 0**, `[kernel]`, the kernel's own, whose threads run kernel
  code only (`kproc`, [`sys/kthread.c`](../../kernel/sys/kthread.c));
- **init**, pid 1, the first user process (see [Booting](booting.md)).
  It is every orphan's parent. If it exits with status 0, the system
  reboots; if it exits otherwise, or a signal kills it, the kernel
  panics.

### Process groups and sessions

UNIX groups processes for *job control*, the shell's control of what runs
on a terminal:

- A **process group** is a job: the processes of one pipeline
  (`ls | grep x`). The terminal sends its signals (`^C`, `^Z`) to the
  whole group, and only its *foreground* group may read from it.
- A **session** is a login: the groups started from one terminal, its
  *controlling terminal*. Its *leader* is the process that made it
  (`setsid()`), usually the login shell; when it exits the terminal
  hangs up.

`struct pgroup` and `struct session` hold them; `session_new()` and
`proc_setpgid()` move processes between them. A group is freed with its
last process, a session with its last group (`pgrp_release()`). A group
is *orphaned* when none of its members has a parent in another group of
the same session (`pgrp_orphaned()`): no shell is left to continue it. A
group that becomes orphaned while one of its members is stopped gets
`SIGHUP` and `SIGCONT`, as POSIX says, so it doesn't stay stopped forever.

### Threads

A thread is a `struct thread` ([`include/sys/thread.h`](../../kernel/include/sys/thread.h)):
its id (`tid`), its process (`owner`), its state, where it sleeps, the
system call it's in, and `arch`, the architecture's part: its registers
and its kernel stack.

| State      | What                                                             |
|------------|------------------------------------------------------------------|
| `RUNNABLE` | running, or ready to run                                         |
| `ISLEEP`   | sleeping, until woken or a signal comes                          |
| `USLEEP`   | sleeping, until woken: signals wait (a disk read halfway)        |
| `ZOMBIE`   | exited (`thread_exit()`), waiting for `thread_join()`            |

Each thread has two stacks: its user stack, in its process's memory, and
a *kernel stack*, which the CPU switches to when the thread enters the
kernel. A system call runs on the kernel stack of the thread that made
it; when it sleeps, its kernel stack stays as it is, with the call
halfway, until it's woken. That's what makes the kernel's code
straightforward: a function that has to wait just sleeps, and goes on
where it was.

### Kernel threads

Some of the kernel's work runs in a thread of its own rather than in a
system call or an interrupt: the network stack's processing, which an
interrupt only hands packets to. These *kernel threads* are threads of
process 0, made with `kthread_create()`; they run a kernel function on
their kernel stack, on the kernel's address space, are scheduled like the
others, and take no signals. There is one today, `net` (`net/netif.c`).

## The scheduler

### Round robin

The scheduler ([`sys/sched.c`](../../kernel/sys/sched.c)) is the simplest
there is: one list of ready threads, `ready`, shared by every CPU,
first in first out. `schedule()` puts the running thread at the end of
the list and runs the one at its head. It runs:

- **on every tick**, every 2 ms on each CPU (see [Interrupts](interrupts.md)):
  so a thread runs for one tick at most, its *time slice*, before the
  next ready one runs;
- **when a thread sleeps or exits**: `kernel_idle()` runs the next ready
  thread, or idles the CPU if there is none.

There are no priorities and no `nice`: every ready thread gets its turn.
A machine that is busy with a few CPU-bound programs and an interactive
one is less responsive than with a scheduler that favours threads that
sleep a lot, as UNIX's always has. It is fair, and it is enough for what
AquilaOS runs.

The one exception is *urgent* threads (`thread->urgent`): woken, they go
to the head of the queue, and run as soon as the interrupt that woke
them returns, before the interrupted user thread goes on
(`sched_thread_wake()`, `sched_urgent`). The network's thread is one, so
that a packet is processed now, not after every ready thread had its
slice.

### Idle

A CPU with nothing to run *idles*: `arch_idle()` runs the architecture's
idle loop on the CPU's own idle stack, which waits for an interrupt
(`curcpu()->idle` is set meanwhile). An interrupt that makes a thread
ready on an idle CPU runs it at once (`sched_idle_wakeup()`), not at the
next tick. A thread made ready while another CPU idles wakes that CPU
with `arch_cpu_kick()` (`sched_cpu_wake()`), so a ready thread doesn't
wait for a busy CPU while another sleeps. See [SMP and the kernel lock](smp.md).

### Time

Each tick charges the running process its 2 ms: to `utime` if it
interrupted user mode, to `stime` if it interrupted the kernel
(`sched_cpu_tick()`). It's sampling, not measuring: a process that
always sleeps before the tick is charged nothing. And since the kernel
runs with interrupts off, a tick that comes during a system call waits
until it returns: kernel time is mostly charged as user time. `times()`
and `/proc` report these.

### Sleeping and waking

A thread that has to wait for something (data on a pipe, a disk block,
a child to exit) sleeps on a *wait queue*, a `struct waitq` of threads
([`sys/waitq.h`](../../kernel/include/sys/waitq.h)) that belongs to what
it waits for: a pipe has one for its readers, a process one for its
parent's `waitpid()`. The pattern is the same everywhere:

```c
while (!what_we_wait_for())
    if (waitq_sleep(&waitq))
        return -ERESTARTSYS;    /* a signal: the call returns */
```

- `waitq_sleep()` ([`sys/waitq.c`](../../kernel/sys/waitq.c))
  puts the thread on the queue, marks it `ISLEEP`, and switches away
  (`arch_sleep()`). A signal the process doesn't block wakes it, and it
  returns `-ERESTARTSYS`: the system call gives up and returns, the
  signal is delivered on the way out, and the call is restarted or fails
  with `EINTR` (see [Signals](signals.md)).
- `waitq_sleep_unint()` sleeps until woken, whatever comes: for
  waits that can't be given up halfway, a disk transfer or a page being
  read in.
- `waitq_wakeup()` wakes every thread on a queue. They become
  ready, and each looks again at what it waits for: a wakeup says
  something changed, not that what it waits for is there. That's why the
  sleep is always in a loop.

`sched_wait(queue, until)` sleeps on a queue with a deadline: the thread
also goes on a list of timed threads, which the boot CPU's tick looks
through, waking those whose time is up (`sched_timer_tick()`). It returns
1 if the time was up. `sched_sleep()` (for `nanosleep()`) is a wait on a
queue nothing else wakes.

`select()` and `poll()` wait for any of several files: the thread
registers the wait queue of each (`poll_register()`) and sleeps on its
own (`thread->poll_wait`); a wakeup of any registered queue wakes it too.

**Nothing here allocates.** A thread is linked into the ready list or
the wait queue it sleeps on through a link inside it (`run_link`: on one
of them at a time), and into the list of timed threads through another
(`timer_link`). The lists are BSD's `TAILQ`, Linux's `list_head`:
[`ds/list.h`](../../kernel/include/ds/list.h), with `list_entry()` to get
the thread back from its link. So going to sleep, waking up and being
scheduled can't fail, even with no memory left (see
[Memory](memory.md)): a list that allocated a node for each entry would
lose a thread whose node it couldn't have.

### Timers

Two more kinds of things happen at a time to come:

- **Callouts** ([`sys/callout.c`](../../kernel/sys/callout.c)): a function
  called at a given time, from the tick (`callout_set()`), or from a
  thread that runs its own list (`callout_set_on()`: TCP's
  retransmission timers run in the network's thread). The caller owns
  the `struct callout`, so arming one never allocates and never fails.
- **Alarms**: a process's real-time timer (`setitimer(ITIMER_REAL)`,
  `alarm()`): `SIGALRM` when it's due, then every interval
  (`sched_alarm()`). Virtual and profiling timers aren't implemented.

## A process's life

### fork

`fork()` makes a copy of the calling process (`proc_fork()`,
[`sys/fork.c`](../../kernel/sys/fork.c)):

1. a new `struct proc` with one thread, and a new pid;
2. the same process group, umask, user, group, signal mask and actions,
   heap, and current directory;
3. its descriptors refer to the same open files (`file_ref()`), sharing
   their offsets, as POSIX says;
4. its address space is a copy, *copy on write*: both processes map the
   same pages read-only, and the first to write one gets its own copy
   (`vm_space_fork()`, see [Memory](memory.md));
5. its one thread is a copy of the calling thread, made by the
   architecture (`arch_proc_fork()`); `fork()` returns 0 in the child
   (set in its registers, `arch_syscall_return()`) and the child's pid in
   the parent (`sys_fork()`'s result).

The child's thread is put on the ready queue; it starts by returning
from `fork()`. Only the calling thread is copied: the child of a process
with several threads has one, as POSIX says.

### exec

`execve()` replaces the program a process runs (`proc_execve()`,
[`sys/execve.c`](../../kernel/sys/execve.c)):

1. The arguments and the environment are copied into the kernel (128 KiB
   at most, `EXEC_ARG_MAX`): the old address space, where they are, is
   about to go.
2. A file that starts with `#!` is a *script*: the interpreter named on
   that line runs instead, with the script's path as an argument
   (four levels of scripts at most).
3. `binfmt_load()` ([`sys/binfmt/binfmt.c`](../../kernel/sys/binfmt/binfmt.c))
   finds a *binary format* that recognises the file. There is one, ELF
   ([`sys/binfmt/elf.c`](../../kernel/sys/binfmt/elf.c)), which takes the
   kernel's own class and machine only (`ELF_MACHINE`, from the
   architecture's `config.h`: a 32-bit i386 program on the i386 kernel, a
   64-bit x86_64 one on the x86_64 kernel, an aarch64 one on the aarch64
   kernel).
4. Then the *point of no return*: the other threads of the process end
   (`proc_threads_exec()`), the old address space is destroyed, and the
   ELF file's `PT_LOAD` segments are mapped in its place. Nothing is read
   yet: each segment maps the file (a `vm_object` of the vnode), and its
   pages are read in when they're first touched. Writable segments are
   private copies (`VM_COPY`); the `.bss` is zeroed.
5. A heap starts after the last segment, a stack (8 MiB,
   `USER_STACK_SIZE`) ends at `USER_STACK`, and the *sigcode* page is
   mapped below it (see [Signals](signals.md)).
6. Descriptors marked close-on-exec are closed. Caught signals go back to
   their default action; ignored ones stay ignored; the mask and the
   pending signals stay.
7. The kernel builds the new program's *initial stack*
   (`exec_stack_build()`, below), the architecture points the thread at
   the program's entry with its stack pointer there
   (`arch_exec_start()`), and the thread starts afresh in user mode
   (`sched_thread_spawn()`).

A failure before step 4 returns an error to the old program. After it,
there is no old program to return to, and the process is killed with
`SIGSEGV`.

The initial stack is the System V ABI's, the same on every architecture
(and what Linux and the BSDs do). At the program's entry, the stack
pointer is 16-byte aligned and points at, one word each:

```
argc
argv[0] ... argv[argc - 1], NULL
envp[0] ... envp[envc - 1], NULL
auxiliary vector: (AT_PAGESZ, 4096) (AT_ENTRY, entry) ... (AT_NULL, 0)
... the strings, at the top of the stack
```

The *auxiliary vector* tells the program about itself and the system:
the page size, its entry point, its user and group IDs
([`aquila/auxv.h`](../../include/uapi/aquila/auxv.h)). It's where a
dynamic linker would find the program's headers, if there were one;
programs are linked statically.

The C library's start code ([`crt0.c`](../../libc/aquila/crt0.c)) reads
it all from the stack pointer, the same way on every architecture, and
calls `main(argc, argv, envp)`; `getauxval()` looks up the vector, as on
Linux. So the architecture's part is small: setting the instruction and
stack pointers.

### Threads

A process makes more threads with `thread_create()`, the C library's
`pthread_create()` underneath ([`sys/sys_proc.c`](../../kernel/sys/sys_proc.c),
`sys_thread_create()`). It gives a user stack, the C library's start
routine, the thread's function and its argument; the architecture makes
the thread's registers and kernel stack (`arch_thread_create()`), and the
thread is put on the ready queue. Its id is the next one in its
process (`tid_last`): threads end in any order, so their count can't be
the next id.

- `thread_exit()` ends the calling thread: it becomes a `ZOMBIE`, its
  exit value kept, and its kernel stack is freed; it stays on its
  process's list until it's joined, or the process exits. The last thread
  to exit ends the process, as `exit(0)`.
- `thread_join()` sleeps on the process's `thread_join` queue until the
  thread is a zombie, writes its value out, and frees what is left of
  it.

A process's threads share its signal mask and pending signals: POSIX
gives each thread its own mask, AquilaOS doesn't (see
[Signals](signals.md)).

### exit and wait

A process ends by calling `exit()`, or by a signal whose action is to
terminate it. Either way `proc_kill()` ([`sys/proc.c`](../../kernel/sys/proc.c))
takes it apart:

1. its timer is disarmed, and its threads end: freed, or, those running
   on another CPU, killed there (see [SMP and the kernel lock](smp.md));
2. its descriptors are closed, its address space destroyed, its current
   directory let go of;
3. its children become init's;
4. if it led a session, its controlling terminal hangs up (its
   foreground group gets `SIGHUP`);
5. it becomes a **zombie**: what's left is the `struct proc`, with its
   exit status and its times, for its parent to collect. Its parent gets
   `SIGCHLD`, and its threads waiting in `waitpid()` are woken.

`waitpid()` looks through the caller's children for one that matches
(a pid, a process group, or any) and has something to report: it
exited, or (with `WUNTRACED`, `WCONTINUED`) it stopped or continued. An
exited child is *reaped* (`proc_reap()`): its times are added to the
parent's `cutime` and `cstime`, its pid freed, its `struct proc` too. If
none has anything to report, the caller sleeps on its `wait_queue`. A
parent that ignores `SIGCHLD`, or set `SA_NOCLDWAIT`, never has zombies:
its children are reaped as they exit.

The status is the classic UNIX encoding: an exit code in bits 8 to 15, a
terminating signal in bits 0 to 6 (the kernel puts it in bits 8 to 15
too, `PROC_EXIT(sig, sig)`), a stop as the signal in bits 8 to 15 and
`0x7f` below, a continue as `0xffff`. The C library's `WIFEXITED()` and
the others read it.

The thread that runs `proc_kill()` for its own process can't free the
kernel stack it runs on. It goes last: it switches to its CPU's idle
stack, frees itself there, and the CPU runs something else
(`arch_cur_thread_kill()`).

## Switching threads

From the kernel's side, a switch is two calls:

- `arch_sleep()`: the current thread stops here, it sleeps, and
  `kernel_idle()` picks the next one; `arch_yield()` is the same for a
  thread that stays ready (the network's thread, after a batch of
  packets) and goes to the end of the queue. When the thread is next chosen, it
  goes on from `arch_sleep()` as if the call had just returned.
- `arch_thread_switch(thread)`: run this thread, where it stopped.
  `arch_thread_spawn(thread)` runs one that never ran, or that just
  started a new program, from the start of its user-mode context
  (`thread->spawned` says which).

What "where it stopped" means, how registers are saved, which stack
holds them: that's the architecture's.

## x86: a thread's registers

On x86 a thread's `arch` is a `struct x86_thread`
([`include/core/arch.h`](../../kernel/arch/i386/include/core/arch.h)):

| Field                     | What                                                    |
|---------------------------|---------------------------------------------------------|
| `kstack_base`, `kstack`   | its kernel stack: where it was allocated, and its top   |
| `eip`/`rip`, `esp`/`rsp`, `ebp`/`rbp`, `eflags`/`rflags` | where it goes on: in the kernel when it was switched away from, or in user mode when it never ran |
| `eax`/`rax`               | a system call's result, for a thread that never ran     |
| `regs`                    | its user-mode registers, saved at the top of its kernel stack when it entered the kernel |
| `fpu_context`, `fpu_enabled` | its FPU and SSE state, once it used them             |

**Kernel stacks** are 8 KiB on i386 and 16 KiB on x86_64
(`KERN_STACK_SIZE`), from `kmalloc()`. A switch sets the TSS's stack
pointer to the next thread's kernel stack (`x86_kernel_stack_set()`):
that's where the CPU puts the user context on the thread's next
interrupt or system call.

### x86: switching

A thread is switched away from by `x86_sleep()` (`arch_sleep()`) or
`x86_preempt()` (the tick, `arch_yield()`), both the `SWITCH_OUT` macro
in [`cpu/cpu.S`](../../kernel/arch/i386/cpu/cpu.S):

1. the thread's registers are pushed on its kernel stack;
2. `x86_thread_saved()` records where it goes on: a label just after,
   with the stack as it is now;
3. `kernel_idle()` or `schedule()` is called, and doesn't return to it.

`arch_thread_switch()` ([`sys/thread.c`](../../kernel/arch/i386/sys/thread.c))
loads the next thread's page tables (`pmap_switch()`), sets the TSS, and
jumps to where that thread went on with `x86_goto()`: its stack pointer,
base pointer and instruction pointer. That's the label in `SWITCH_OUT`;
the registers are popped and `x86_sleep()` returns, in the other thread.
There is no "switch from A to B" routine saving one and loading the
other: saving happens when a thread stops, loading when it's chosen, and
the scheduler runs in between on the stack of the thread that stopped.

A thread that never ran starts with `arch_thread_spawn()`: its page
tables, the TSS, then `x86_goto()` to the top of its kernel stack, where
`thread_spawn()` lets go of the kernel lock and `x86_jump_user()` `iret`s
to its user-mode registers.

### x86: fork

`arch_proc_fork()` ([`sys/fork.c`](../../kernel/arch/i386/sys/fork.c))
copies the parent's kernel stack whole into the child's, so the system
call's frame, the parent's user registers, is in the same place on it.
The child is marked as having run, and set to go on at
`x86_fork_return` (in `cpu.S`), on that frame: when it is first chosen,
it lets go of the kernel lock and returns to user mode through the copied
frame, as the parent will, with 0 in `eax`.

### x86: the FPU

The FPU and SSE registers are big (512 bytes, `fxsave`), and most
switches are between threads that don't use them, so they're switched
*lazily* ([`cpu/fpu.c`](../../kernel/arch/i386/cpu/fpu.c)):

- on every switch the FPU is turned off (`CR0.TS`, `x86_fpu_disable()`);
- the thread's first FPU instruction traps (*device not available*,
  vector 7), and `x86_fpu_trap()` saves the previous owner's state, loads
  this thread's (or a clean one, the first time), and turns it back on;
- a thread switched away from with its state in the FPU saves it then
  (`x86_fpu_switch_out()`): with several CPUs, it may run on another one
  next.

Each CPU remembers whose state its FPU has (`fpu_thread`). A forked
child gets a copy of its parent's state; a signal handler starts with a
clean FPU, and `sigreturn` puts the old state back.

## aarch64: a thread's registers and switching

On aarch64 a thread's `arch` is a `struct aarch64_thread`
([`include/core/arch.h`](../../kernel/arch/aarch64/include/core/arch.h)):
its kernel stack (16 KiB), `ctx`, where it goes on in the kernel, `regs`,
its user-mode registers saved on its kernel stack, `pc`, `sp` and `x0` to
`x2` for a thread that never ran, and `fpsimd`, its FP/SIMD state.

Switching works as on x86, with `setjmp()` and `longjmp()` in place of
`SWITCH_OUT` ([`cpu/switch.S`](../../kernel/arch/aarch64/cpu/switch.S)):
`arch_sleep()` and `aarch64_preempt()` save the registers a call keeps,
the frame pointer, the link register and the stack pointer
(`aarch64_ctx_save()`, which returns 0), then call `kernel_idle()` or
`schedule()`. `arch_thread_switch()` ([`sys/thread.c`](../../kernel/arch/aarch64/sys/thread.c))
loads the next thread's page tables, then its `ctx` (`aarch64_ctx_load()`),
and `aarch64_ctx_save()` returns again, with 1, in that thread. There is
no TSS: user mode comes back on the stack the kernel left in `SP_EL1`
when it returned to it, the top of the thread's kernel stack. A thread
that never ran goes to user mode with `aarch64_jump_user()`, an `eret`.

`arch_proc_fork()` ([`sys/fork.c`](../../kernel/arch/aarch64/sys/fork.c))
copies only the fork call's saved registers, to the top of the child's
kernel stack, and sets the child to go on at `aarch64_fork_return` (in
`cpu/vectors.S`), which lets go of the kernel lock and returns to user
mode from that copy, with 0 in `x0`.

The FP/SIMD registers (`v0` to `v31`, `FPSR`, `FPCR`: 528 bytes) are
switched lazily too ([`cpu/fpsimd.c`](../../kernel/arch/aarch64/cpu/fpsimd.c)).
A switch sets `CPACR_EL1.FPEN` so that user mode's use traps, unless the
registers hold the thread's values already (`aarch64_fpsimd_switch()`).
At the trap, `aarch64_fpsimd_trap()` saves the previous owner's values
(`fp_thread`), loads the thread's (zeros, the first time), and lets user
mode use them. There is no saving when a thread is switched away from:
aarch64 runs on one CPU so far, so the state never moves to another. A
forked child gets a copy of its parent's state, a new program starts
with zeros, and a signal handler starts with the registers as they were.

## Where it is

| File                                        | Layer   | What                                         |
|---------------------------------------------|---------|----------------------------------------------|
| `kernel/include/sys/proc.h`                 | kernel  | processes, groups, sessions                  |
| `kernel/include/sys/thread.h`               | kernel  | threads                                      |
| `kernel/sys/proc.c`                         | kernel  | pids, groups, sessions, `proc_kill()`, `proc_reap()`, descriptors |
| `kernel/sys/thread.c`                       | kernel  | threads                                      |
| `kernel/sys/waitq.c`, `include/sys/waitq.h` | kernel  | wait queues, `poll_register()`               |
| `kernel/include/ds/list.h`                  | kernel  | lists linked through their entries           |
| `kernel/sys/kthread.c`                      | kernel  | process 0 and kernel threads                 |
| `kernel/sys/sched.c`                        | kernel  | the scheduler, the tick, timed waits, alarms |
| `kernel/sys/callout.c`                      | kernel  | callouts                                     |
| `kernel/sys/fork.c`                         | kernel  | `fork()`                                     |
| `kernel/sys/execve.c`                       | kernel  | `execve()`, scripts                          |
| `kernel/sys/binfmt/`                        | kernel  | binary formats: ELF                          |
| `kernel/sys/sys_proc.c`                     | kernel  | `exit`, `waitpid`, threads, `setpgid`, `setsid` |
| `kernel/sys/sys_signal.c`                   | kernel  | `nanosleep`, `setitimer`                     |
| `kernel/arch/i386/include/core/arch.h`      | x86     | `struct x86_thread`, stack sizes, `USER_STACK` |
| `kernel/arch/i386/sys/thread.c`             | x86     | making, spawning, switching, killing threads |
| `kernel/arch/i386/sys/sched.c`              | x86     | the idle loop, the tick's handler, killing the current thread |
| `kernel/arch/i386/sys/fork.c`               | x86     | the child's thread                           |
| `kernel/arch/i386/sys/execve.c`             | x86     | where a new program starts: its instruction and stack pointers |
| `kernel/arch/i386/sys/proc.c`               | x86     | init's thread                                |
| `kernel/arch/i386/cpu/cpu.S`                | x86     | `SWITCH_OUT`, `x86_goto`, `x86_jump_user`, `x86_fork_return` |
| `kernel/arch/i386/cpu/fpu.c`                | x86     | the lazy FPU                                 |
| `kernel/arch/aarch64/include/core/arch.h`   | aarch64 | `struct aarch64_thread`, stack sizes, `USER_STACK` |
| `kernel/arch/aarch64/sys/thread.c`          | aarch64 | making, spawning, switching, killing threads |
| `kernel/arch/aarch64/sys/sched.c`           | aarch64 | the idle loop, the tick, killing the current thread |
| `kernel/arch/aarch64/sys/fork.c`            | aarch64 | the child's thread                           |
| `kernel/arch/aarch64/sys/execve.c`          | aarch64 | where a new program starts                   |
| `kernel/arch/aarch64/cpu/switch.S`          | aarch64 | `aarch64_ctx_save`, `aarch64_ctx_load`, `aarch64_jump_user` |
| `kernel/arch/aarch64/cpu/fpsimd.c`          | aarch64 | the lazy FP/SIMD registers                   |

## References

- *The Design of the UNIX Operating System*, Maurice J. Bach: chapters 6
  to 8, processes, `fork`, `exec`, `exit`, `wait`, sleeping and waking,
  and the classic scheduler.
- *Lions' Commentary on UNIX 6th Edition*, John Lions: `swtch()`,
  `sleep()` and `wakeup()` in their original form, from which AquilaOS's
  wait queues descend.
- *The Design and Implementation of the FreeBSD Operating System*,
  McKusick, Neville-Neil and Watson: chapter 4, processes, threads and
  scheduling; process 0 and kernel threads.
- POSIX.1-2017 (*The Open Group Base Specifications, Issue 7*): `fork()`,
  `execve()`, `_exit()`, `waitpid()`, `setpgid()`, `setsid()`, and
  *General Terminal Interface* (11.1.3, 11.1.4) for sessions, process
  groups and orphaned groups.
- *Tool Interface Standard (TIS) Executable and Linking Format (ELF)
  Specification*, version 1.2: program headers and `PT_LOAD` segments.
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3:
  *Task Management* (the TSS) and *System Programming for Instruction Set
  Extensions* (13.4, `CR0.TS` and saving the FPU lazily).
- The OSDev wiki's pages on [Context Switching](https://wiki.osdev.org/Context_Switching),
  [Scheduling Algorithms](https://wiki.osdev.org/Scheduling_Algorithms)
  and [ELF](https://wiki.osdev.org/ELF).
