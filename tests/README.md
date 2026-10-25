# Kernel tests

Boots the kernel in QEMU with small test programs as `/init` and checks
what they report on the serial port.

```
tests/run.sh            # build the kernel and run everything
tests/run.sh cpu ext2   # run some tests
tests/run.sh -n         # reuse the kernel image from the last build
tests/run.sh -v         # print the serial log of failed tests
tests/run.sh -a i386    # the i386 kernel and system (default: x86_64)
tests/run.sh -a aarch64 # the aarch64 kernel on QEMU's virt machine: the aarch64 and oom tests
tests/run.sh -k apic=off    # more for the kernel's command line, every boot
tests/run.sh -c 4       # QEMU with 4 CPUs, every boot (but the irq and smp tests)
tests/run.sh -j 4       # 4 tests at once (clock, irq, smp and memory alone, after)
```

The kernel is built with the host compiler through
[`host/x86_64-pc.mk`](host/x86_64-pc.mk) (or
[`host/i386-pc.mk`](host/i386-pc.mk)), the regular configuration with
`gcc`/`ld` instead of the cross toolchain. Any gcc with 32-bit support
works. On aarch64 it is the regular configuration,
[`configs/aarch64-virt.mk`](../configs/aarch64-virt.mk), built with
`clang`. The libc, system and disk tests use the system built with the
cross toolchain (`make`, `make CONFIG=i386-pc`), the tcc, curl and bash
tests its ports (`make ports`), and are skipped without them.

Requirements: `gcc`, `ld`, `cpio`, `qemu-system-x86_64` and `python3`
(the clock, net, tcp, curl and minix tests' helpers on the host), and
`qemu-system-i386` for i386; for aarch64 `clang`, `ld.lld`,
`llvm-objcopy` and `qemu-system-aarch64`, the tests built with clang.
These tests are skipped without what they need: ext2 without `mke2fs`,
`e2fsck` and `debugfs`, minix without `mkfs.minix` and `fsck.minix`,
disk without `sfdisk`, `mkfs.fat`, mtools, `mke2fs`, `e2fsck`,
`debugfs` and `fakeroot`. The curl test also needs `openssl`.

Build output goes to `tests/out/x86_64` (`tests/out` for i386,
`tests/out/aarch64` for aarch64).

## Tests

| test       | what it checks |
|------------|----------------|
| `ata`      | a disk on the first ATA channel is detected, over several boots |
| `cpu`      | DF and segment registers are sanitized on kernel entry, user exceptions and port I/O raise signals, SSE works, entry stack alignment, the initial stack (argc, argv and envp with their NULLs, the auxiliary vector's page size and entry point) |
| `syscalls` | kernel addresses are rejected (`EFAULT`), and user memory that isn't mapped (the process goes on, a read's bytes stay, a buffer running into it is used up to it), descriptor and signal checks, wait, threads, pipes (and how fast a ping-pong through them goes: no timer tick waited for), unix sockets, bad ELF files, descriptor flags and close-on-exec, open files shared by `dup` and `fork` (offset, status flags) and kept open by a read waiting on one while its descriptor is closed, symlinks, signal masks, process groups, orphans (init's to wait for; a stopped orphaned group gets `SIGHUP` and `SIGCONT`, the terminal's stops and background reads in one don't stop it), copy-on-write after fork, the `*at()` calls, links, rename, permissions and attributes, paths walked vnode by vnode (`..` after a symbolic link, the cwd and an open directory renamed, `..` through mounts, search permission, a removed cwd, trailing slashes), `SIGPIPE`, `nanosleep`, clocks and `times`; `nanosleep`, `poll` and `select` stopped and continued keep their deadline; signals delivered on the way back to user mode: the SSE state kept across a handler, the handler's signal blocked (`SA_NODEFER`), `sa_mask`, `SA_RESETHAND`, nested handlers, `sigsuspend`, a read stopped and continued made again, a forged `sigreturn` frame |
| `memory`   | with 6 GiB of RAM (2 GiB on i386, which uses 768 MiB), one big mapping (1 GiB, 128 MiB on i386) then 4.5 GiB (512 MiB) of anonymous memory filled, every page with where it is, and read back: memory above 4 GiB, the kernel's small objects (slabs) and its hashmaps for millions of pages |
| `oom`      | out of memory, with 128 MiB: a process maps 1 GiB and touches it all, or `read()`s `/dev/zero` into it; the page fault that finds no memory kills it (`SIGKILL`), three times each, its memory back each time, and `fork` works after (x86 and aarch64) |
| `aarch64`  | the generic kernel on aarch64 (`-a aarch64`): user mode and `svc` system calls, the PL011 console, fork and copy on write, exec with arguments, pipes, anonymous memory, page faults (`SIGSEGV`), `EFAULT`, a signal handler and sigreturn, the tick, `uname`'s machine, each thread's FP/SIMD registers, and the e1000 on PCI Express (ECAM) pinging QEMU's gateway |
| `kvmem`    | the kernel's allocator: its self-test at boot (`kvmem.test`: objects of many sizes on pages of their own, their guard pages unmapped, none overwriting another, freed out of order and the arena merged back as it was); forks, execs and threads leave no object behind; a write past an object's end faults at its guard page (`kvmem.guard`) |
| `clock`    | the clock from each of its sources, QEMU's options choosing (`clock=` too): the TSC (invariant), the HPET, the PIT's ticks without an HPET; it never goes back, its resolution (finer than a tick but with the PIT), `nanosleep`, and 2 s as the host counts them (`clock/stamp.py` timestamps the serial lines) |
| `irq`      | interrupts through the APIC and through the 8259 (`apic=off`): `/proc/interrupts` (the controller, the routes), the timer's 500 ticks a second, the NIC's IRQ when a frame comes: the 82540EM's IRQ 11 (level-triggered under the APIC), the 82574L's MSI (its line with the 8259), on QEMU's pc and q35 |
| `smp`      | 4 CPUs, on QEMU's pc and q35 (`smp/init.c`): threads run at the same time (each sees the others' counters move between two of its reads), a page unmapped while a thread on another CPU reads it faults by the time `munmap` returns, `fork` while a thread writes on another CPU leaves the child's copy as it was, processes killed or exiting while their threads run on other CPUs, `exec` while they do (they end first), a signal to a process running on another CPU, the FPU of threads moving between CPUs; the boot CPU only with `smp=off`, and with `apic=off` |
| `net`      | twice, the 82540EM (`e1000`, its line) and the 82574L (`e1000e`, MSI); the e1000 and IPv4: a peer on the host (`net/peer.py`, QEMU's `-netdev socket`) checks the answers to ARP, pings (100 of them, around the rings) and a datagram to a closed port; the guest's program checks process 0 and its network thread (`[net]`, which takes the frames up the stack) in `/proc/0/status`, UDP and ping sockets over loopback (binding, `getsockname`, `FIONREAD`, timeouts, `SO_ERROR` from "port unreachable", `SO_BROADCAST`) and the interfaces' `SIOCGIF*` requests, then talks to the peer: UDP after resolving its address with ARP, a ping, a refused datagram, one through the gateway and a broadcast; TCP over loopback (handshake, data both ways, half-close, reset, `poll`, non-blocking `connect` and `accept`, `SIGPIPE`, a megabyte each way with a child), and with the peer playing TCP segment by segment: a SYN retransmitted, data out of order, fast retransmit and recovery, a retransmission timeout, a zero window probed, a reset, RST for a closed port |
| `tcp`      | TCP against another one: QEMU's user network has its own (from BSD's), the guest (`tcp/init.c`) and `tcp/host.py` exchange megabytes through it: 2 MiB out, an echo server for 4 MiB and 5 connections at once |
| `ext2`     | writes through singly, doubly and triply indirect blocks (a sparse file past 64 MiB), appends, directories, renames, links, truncating on an ext2 image, then `e2fsck` and a byte compare on the host; the disk has no partition table, so its first partition isn't there (`ENXIO`) |
| `minix`    | a minix v1 image (`mkfs.minix -1`): a file through the singly and doubly indirect zones, a sparse one (its holes read zeros), truncating (the zones past the end freed), then `fsck.minix` on the host |
| `libc`     | errno and signal numbers, stdio, directories (`scandir`, `alphasort`), fork/wait, crypt, passwd/shadow and group, truncate, signal masks, the `*at()` calls, `realpath`, `ttyname`, sleeping, `SIGPIPE`, C99 `printf` formats and TCP over loopback (with `stdio` on a socket, `SOCK_NONBLOCK`, `accept4`, `sendmsg`/`recvmsg`), `err(3)`, and `getauxval()`, the resolver (`/etc/hosts`, `/etc/services`, and a DNS server of the test's own on 127.0.0.1: several addresses, a CNAME, the search domain, a truncated answer and TCP, no such name, PTR) through newlib (needs `make toolchain`) |
| `system`   | the initrd built from source (with contrib/, without ports/): logins on the serial console, then dash runs commands, pipelines, `$(...)`, a long line, `^C`, `/dev/tty` and sbase tools, `nc` over loopback, and the `dhcp` service enabled (sdhcp gets the address from QEMU's user network, a ping goes through) (needs `make toolchain` and `make`) |
| `disk`     | the system installed on a hard disk image (`mk/hdd.sh`): booted from it with Limine twice, a file written on the first boot is read on the second (`mk/hdd-kernel.sh` and `mk/hdd-update.sh` put the kernel and an updated system on it in between), `e2fsck` is clean (needs `make toolchain` and `make`) |
| `tcc`      | the tcc port (`make ports`) on AquilaOS: compiles and runs programs against the C library and libm (skipped without the port) |
| `curl`     | the curl port (`make ports`) on AquilaOS, against HTTPS servers on the host with a CA made for the test: the page fetched over TLS 1.3, a certificate for another name and one from an unknown CA refused (skipped without the port) |
| `bash`     | the bash port (`make ports`) on AquilaOS: a script of its features run as a program (arrays, strings, functions, process substitution, `pipefail`, background jobs and `wait`, traps, extended globs), its version and the machine type it reports (skipped without the port) |

A test fails on a `FAIL` line, a kernel panic, a guest reset (QEMU runs
with `-no-reboot`) or if it doesn't finish in time.

## Writing a test

Add `tests/<name>/init.c` using [`lib/aqtest.h`](lib/aqtest.h): no libc,
syscalls through `SC()`, results through `check()`.

```c
#include "../lib/aqtest.h"

void aq_main(void)
{
    aq_begin("example");    /* mounts devfs, opens /dev/ttyS0 */

    check("getpid", SC(SYS_getpid, 0, 0, 0) > 0, 1);

    aq_end();               /* prints the summary, never returns */
}
```

`aq_argc` and `aq_argv` are the program's arguments, read from its
initial stack (the System V ABI's: `argc`, `argv`, `envp`, then the
auxiliary vector); `aq_check_initial_stack()` checks that stack.

Then, in `run.sh`, add a `test_<name>` function, the name to
`ALL_TESTS`, and the name to the loop that builds the test programs'
initrds (`for name in cpu syscalls ...; do`). The initrd has `/init`,
and empty `/dev`, `/mnt`, `/proc` and `/tmp` directories; the initramfs
is read-only, mount tmpfs to create files.
