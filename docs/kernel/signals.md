# Signals

A *signal* is UNIX's way of telling a process that something happened:
the user pressed `^C` (`SIGINT`), a child exited (`SIGCHLD`), a timer
went off (`SIGALRM`), the process wrote to a pipe no one reads
(`SIGPIPE`), or it did something the CPU refused (`SIGSEGV`, `SIGFPE`).
Each signal has an *action*: the default one (end the process, stop it,
or nothing), ignoring it, or a *handler*, a function of the process that
runs when the signal comes, in the middle of whatever it was doing, and
then returns to it.

Signals are an old and awkward interface, and the hard part for a kernel
isn't sending them, it's delivering them: running a function of the
program at an arbitrary point, then going back as if nothing happened,
and interrupting system calls that are waiting. This document covers how
AquilaOS does it. Which signals exist, when they're pending, and what
happens to system calls they interrupt is the kernel's design; running a
handler means building a frame on the user stack out of the CPU's
registers, which is the architecture's, described at the end for x86
and aarch64 (see [The kernel and the machine](architecture.md)).

## The kernel's side

### Signals and their actions

AquilaOS has 31 signals, numbered as newlib's for i386 (`SIGHUP` is 1,
`SIGKILL` 9, `SIGUSR2` 31: [`include/uapi/aquila/signal.h`](../../include/uapi/aquila/signal.h)).
A process has, for each signal ([`include/sys/proc.h`](../../kernel/include/sys/proc.h)):

- its **action**, `sigaction[sig]`: `SIG_DFL`, `SIG_IGN`, or a handler,
  with the flags and the mask of the `sigaction()` call that set it;
- a bit in its **pending** set, `sig_pending`: sent, not yet delivered;
- a bit in its **mask**, `sigmask`: blocked, it stays pending until
  unblocked.

The sets are bit masks, signal *n* being bit `1 << n`. A signal sent
again while pending is delivered once: signals aren't counted, as
everywhere.

The default actions ([`sys/signal.c`](../../kernel/sys/signal.c),
`sig_default_action[]`):

| Default action | Signals                                                     |
|----------------|-------------------------------------------------------------|
| terminate      | `SIGHUP`, `SIGINT`, `SIGKILL`, `SIGPIPE`, `SIGALRM`, `SIGTERM`, `SIGUSR1`, `SIGUSR2`, `SIGPOLL`, `SIGPROF`, `SIGVTALRM`, `SIGLOST` |
| abort          | `SIGQUIT`, `SIGILL`, `SIGTRAP`, `SIGABRT`, `SIGEMT`, `SIGFPE`, `SIGBUS`, `SIGSEGV`, `SIGSYS`, `SIGXCPU`, `SIGXFSZ` |
| stop           | `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`                  |
| continue       | `SIGCONT`                                                   |
| ignore         | `SIGCHLD`, `SIGURG`, `SIGWINCH`                             |

*Abort* is where UNIX writes a core file; AquilaOS writes none, and
aborts as it terminates. `SIGKILL` and `SIGSTOP` can't be caught,
ignored or blocked (`SIG_UNMASKABLE`).

The mask and the pending set are the process's, shared by its threads.
POSIX gives each thread its own mask, and lets a signal be sent to one
thread; AquilaOS doesn't, and delivers a signal to whichever of the
process's threads returns to user mode first.

### Sending

Every way of sending a signal ends in `signal_to()`:

| Who                       | How                                                     |
|---------------------------|---------------------------------------------------------|
| `kill()`                  | `signal_send()`: a process, a process group, or every process but init and the caller |
| the terminal              | `^C`, `^\`, `^Z` to its foreground group; `SIGTTIN`, `SIGTTOU` to a background group that reads or writes it; `SIGHUP` when it hangs up (`dev/tty/generic.c`) |
| a child                   | `SIGCHLD` to its parent when it exits, stops or continues |
| the timer                 | `SIGALRM` (`sched_timer_tick()`)                        |
| a write with no reader    | `SIGPIPE`, to the writer (pipes, sockets)               |
| a fault                   | `signal_force()`: `SIGSEGV`, `SIGILL`, `SIGFPE`, `SIGBUS`, `SIGTRAP`, from the architecture's trap handler |
| `Ctrl-Alt-Del`            | `SIGINT` to init, if it asked for it (`reboot(RB_DISABLE_CAD)`); otherwise the machine restarts (`sys/reboot.c`) |

`signal_to()` doesn't run anything. It:

1. does the parts of job control that can't wait: a stop signal discards
   a pending `SIGCONT`, and `SIGCONT` or `SIGKILL` discards pending stops
   and continues a stopped process at once (whatever `SIGCONT`'s action
   is); for `SIGCONT`, it tells the parent;
2. discards the signal if delivering it would do nothing: its action is
   `SIG_IGN`, or it's `SIG_DFL` and the default is to ignore, or to
   continue (`SIGCONT`'s, done already);
3. otherwise sets its bit in the pending set;
4. if the process doesn't block it, wakes its threads that sleep in an
   *interruptible* sleep (`ISLEEP`), and makes those running on another
   CPU enter the kernel now (`sched_thread_kick()`).

Process 0, the kernel's, takes no signals; a zombie takes none either.

`signal_force()` is for faults: the instruction that faulted would fault
again if the signal were blocked or ignored, so if it is either, it
unblocks the signal and resets its action to the default first. A program that blocks
`SIGSEGV` and then faults dies.

### Interrupting system calls

A thread that sleeps in a system call (reading a terminal, waiting for a
child) sleeps with `waitq_sleep()` (see [Processes and threads](processes.md)).
Woken by a signal, it returns `-ERESTARTSYS`, and the system call gives
up and returns it, undoing nothing: every system call is written so that
returning before it did anything is safe.

On the way back to user mode, after the signal was taken, the call is
either *restarted*, made again from the start as if the program had just
made it, or fails with `EINTR`:

| The call returned | A handler ran, `SA_RESTART`                    | A handler ran, no `SA_RESTART` | No handler (a stop, an ignored signal) |
|-------------------|-------------------------------------------------|---------------------------------|----------------------------------------|
| `-ERESTARTSYS`    | restarted                                       | `EINTR`                         | restarted                              |
| `-ERESTARTNOHAND` | `EINTR`                                         | `EINTR`                         | restarted                              |

`ERESTARTNOHAND` is for the calls POSIX says fail with `EINTR` whatever
`SA_RESTART` says: `sigsuspend()`, `select()`, `poll()`, `nanosleep()`.
User space never sees either code. A call restarted after a stop knows it
is a restart (`thread->restarting`): `nanosleep()`, `select()` and
`poll()` then keep the deadline they had, rather than waiting their full
time again.

The names, and the scheme, are Linux's.

### Delivering

A signal is delivered at one place only: **on the way back to user
mode**, from a system call, an interrupt or an exception. The
architecture's return path calls `signal_take()`, which looks at the
signals that are pending and not blocked, `SIGKILL` first, then the
lowest numbered, and for each:

- `SIG_IGN`: dropped (it was ignored after it was sent);
- terminate or abort by default: the process ends (`signal_exit()`), its
  status saying which signal killed it;
- stop by default: the process stops (`signal_stop()`, below);
- continue or ignore by default: nothing (continuing was done when it
  was sent);
- a handler: `signal_take()` returns the signal, and the architecture
  runs the handler.

Delivering only there is what keeps signals simple for the kernel: it
never has to stop in the middle of its own code to deal with one. A
system call either finishes, or, if it sleeps, is woken and returns, and
the signal is taken after. The kernel stack is never left halfway.

### Running a handler

To run a handler, the architecture changes the user-mode registers the
thread is about to return to (`deliver()`):

1. the registers the thread had, the current mask and the FPU state are
   saved in a *signal frame* on the user stack;
2. the return address on the stack is the **sigcode** page's;
3. the instruction pointer becomes the handler's, the stack pointer the
   frame's, and the arguments are the signal number and a `siginfo`
   (with the faulting address for `SIGSEGV`, `SIGBUS`, `SIGILL` and
   `SIGFPE`);
4. the mask becomes the old one plus the action's `sa_mask` and the
   signal itself (unless `SA_NODEFER`); with `SA_RESETHAND` the action
   goes back to the default.

The thread returns to user mode, in the handler. When the handler
returns, it returns into the sigcode page, which makes one system call,
`sigreturn()`: the architecture reads the frame back from the user
stack, and puts the registers, the mask and the FPU state back as they
were (`arch_sigreturn()`). The thread returns to user mode where it was
when the signal came.

The **sigcode** page is mapped in every process, read and execute only,
below the stack (`SIGCODE_ADDR`), by `exec` (`arch_sigcode_map()`). It's
one page of the kernel's, the same physical page in every process. It
means nothing in user space has to know about `sigreturn`: the C library
passes the handler to `sigaction()` as it is, with no *restorer* of its
own (Linux's way).

The frame is written with `copyout()` and read back with `copyin()`: a
stack that isn't there, or a frame that was scribbled over, is the
process's fault, not the kernel's. One that can't be written kills the
process with `SIGSEGV`; one that can't be read back gets it `SIGSEGV`.
A frame's registers are checked before they're loaded: user-mode
segments only, no flags user mode couldn't set itself.

Every handler gets the `siginfo`, as with `SA_SIGINFO`; its third
argument, the `ucontext`, is `NULL`. There is no alternate signal stack
(`sigaltstack()`).

### Stopping and continuing

A process stops by taking a stop signal (`SIGSTOP`, `SIGTSTP` from `^Z`,
`SIGTTIN`, `SIGTTOU`) on its way back to user mode: `signal_stop()` marks
it stopped, tells its parent (`SIGCHLD`, unless `SA_NOCLDSTOP`, and a
wakeup in `waitpid()` with `WUNTRACED`), and the thread sleeps on the
process's `stop_queue`, uninterruptibly: only continuing ends it.
`SIGCONT` and `SIGKILL` continue it when they're sent (`signal_to()`),
and its parent's `waitpid()` with `WCONTINUED` hears about it.

The terminal's stop signals (`SIGTSTP`, `SIGTTIN`, `SIGTTOU`) don't stop
a process whose group is orphaned, as POSIX says: no shell is left to
continue it. `SIGSTOP` always does.

### Waiting for a signal

- `sigsuspend(mask)` replaces the mask and sleeps until a signal it
  doesn't block comes. The mask it replaced is kept by the thread
  (`thread->sig_oldmask`): if a handler runs, its frame saves that mask,
  and `sigreturn()` puts it back; if no handler runs (a stop), it's put
  back on the way out.
- `pselect()` is `select()` with a mask while it waits, kept the same
  way.
- `sigprocmask()` changes the mask; a signal it unblocks that was pending
  is delivered before the call returns to user mode. `kill()` of the
  caller itself is the same: the signal is delivered before `kill()`
  returns.

## x86: the return to user mode

Every return to user mode on x86 goes through `x86_return_to_user()`
([`sys/signal.c`](../../kernel/arch/i386/sys/signal.c)): after a system
call (`arch_syscall()`), a device interrupt, an exception. It's a no-op
unless the saved context is user mode's (`cs`'s privilege level is 3).
It calls `signal_take()`, then, for a system call:

- **restarting** a call is going back two bytes, to the `int $0x80`
  instruction, with the call's number back in `eax` (it has the result
  now): the program makes the call again, with the same arguments, still
  in the other registers;
- **`EINTR`** is writing `-EINTR` into `eax`.

Then, if there's a handler, `deliver()` builds its frame.

## x86: the signal frame

The frame, on the user stack below where the thread was (and below the
128-byte *red zone* on x86_64, which code may use without moving the
stack pointer), 16-byte aligned:

```c
struct sigframe {
    struct aquila_siginfo info; /* the handler's siginfo */
    sigset_t mask;              /* sigreturn restores it */
    uint32_t fpu;               /* fxsave below is the thread's state */
    struct x86_regs regs;       /* the interrupted context */
    uint8_t fxsave[512] __aligned(16);
};
```

Below it, what a `call` would have left: on i386 the return address
(the sigcode page) and the handler's three arguments on the stack
(signal, `&info`, `NULL`), padded so the stack is aligned as the ABI
wants at a function's entry; on x86_64 the return address only, the
arguments in `rdi`, `rsi`, `rdx`. The handler starts with a clean FPU,
and the direction flag clear.

The sigcode page holds three instructions:

```
mov $SYS_sigreturn, %eax
int $0x80
ud2
```

`arch_sigreturn()` finds the frame from the stack pointer (the handler's
`ret` popped the return address), reads it back with `copyin()`, and
replaces the saved user context with the frame's `regs`, with `cs` and
`ss` set to user mode's, and only the flags user code may set
(`USER_FLAGS`: not `IF`, not `IOPL`). `arch_syscall()` doesn't treat
`sigreturn` as an interrupted call: `eax` is the frame's, not a result.

The sigcode page is a `vm_object` of its own with a pager that makes the
page the first time any process touches it (`sigcode_in()`): every
process maps the same object.

## aarch64: the return to user mode and the frame

aarch64 does the same in [`sys/signal.c`](../../kernel/arch/aarch64/sys/signal.c).
Every return to user mode goes through `aarch64_return_to_user()`, a
no-op unless the saved context is EL0's. Restarting a call is going back
four bytes, to the `svc`, with its first argument back in `x0` from
`orig_x0` (the number is still in `x8`); `EINTR` is writing `-EINTR`
into `x0`.

The frame, on the user stack below where the thread was (AAPCS64 has no
red zone), 16-byte aligned:

```c
struct sigframe {
    struct aquila_siginfo info;
    sigset_t mask;
    uint64_t fpsimd_valid;      /* fpsimd below is the thread's state */
    struct aarch64_regs regs;   /* the interrupted context */
    uint8_t fpsimd[FPSIMD_STATE_SIZE] __aligned(16);
};
```

The handler gets its arguments in `x0`, `x1`, `x2` (signal, `&info`,
`NULL`), and its return address in `x30`, the sigcode page's. Unlike
x86's, it starts with the FP/SIMD registers as they were, not cleared.
The sigcode page holds `mov x8, #SYS_sigreturn`, `svc #0` and `udf #0`;
`sigcode_in()` also makes the instruction cache see it.

`arch_sigreturn()` reads the frame at the stack pointer, puts the
registers back with only the flags user code may set (`USER_FLAGS`:
`N`, `Z`, `C`, `V`; the mode is EL0's and the interrupts unmasked), and
the FP/SIMD state back (`aarch64_fpsimd_restore()`).

## Where it is

| File                                | Layer   | What                                         |
|-------------------------------------|---------|----------------------------------------------|
| `include/uapi/aquila/signal.h`      | kernel  | the numbers, `siginfo`, `sigaction` and its flags (shared with the C library) |
| `kernel/include/sys/signal.h`       | kernel  | the interface, `ERESTARTSYS`, `ERESTARTNOHAND` |
| `kernel/sys/signal.c`               | kernel  | default actions, sending, taking, stopping   |
| `kernel/sys/sys_signal.c`           | kernel  | `kill`, `sigaction`, `sigprocmask`, `sigsuspend`, `sigreturn` |
| `kernel/sys/sys_poll.c`             | kernel  | `select`'s mask                              |
| `kernel/sys/proc.c`                 | kernel  | `SIGCHLD`, orphaned groups                   |
| `kernel/dev/tty/generic.c`          | kernel  | the terminal's signals                       |
| `kernel/arch/i386/sys/signal.c`     | x86     | `x86_return_to_user()`, restarts, the frame, sigcode, `sigreturn` |
| `kernel/arch/i386/sys/syscall.c`    | x86     | the return path after a system call          |
| `kernel/arch/i386/cpu/isr.c`        | x86     | exceptions to signals                        |
| `kernel/arch/i386/cpu/fpu.c`        | x86     | the FPU state in and out of frames           |
| `kernel/arch/aarch64/sys/signal.c`  | aarch64 | `aarch64_return_to_user()`, restarts, the frame, sigcode, `sigreturn` |
| `kernel/arch/aarch64/sys/syscall.c` | aarch64 | the return path after a system call          |
| `kernel/arch/aarch64/cpu/trap.c`    | aarch64 | exceptions to signals                        |
| `kernel/arch/aarch64/cpu/fpsimd.c`  | aarch64 | the FP/SIMD state in and out of frames       |

## References

- POSIX.1-2017 (*The Open Group Base Specifications, Issue 7*): *Signal
  Concepts* (2.4), `sigaction()`, `sigprocmask()`, `sigsuspend()`,
  `pselect()`, `kill()`, and *Interruption of system calls*.
- *Advanced Programming in the UNIX Environment*, W. Richard Stevens and
  Stephen A. Rago: chapter 10, signals as programs see them, and the
  history of their unreliable first versions.
- *The Design of the UNIX Operating System*, Maurice J. Bach: chapter 7.2,
  signals in the kernel, checked on the way back to user mode.
- *The Linux Programming Interface*, Michael Kerrisk: chapters 20 to 22,
  including restarting system calls (21.5).
- System V Application Binary Interface, *Intel386 Architecture Processor
  Supplement* and *AMD64 Architecture Processor Supplement*: the stack's
  alignment at a function's entry, and the red zone.
- *Procedure Call Standard for the Arm 64-bit Architecture* (AAPCS64):
  the arguments' registers, the link register, the stack's alignment.
