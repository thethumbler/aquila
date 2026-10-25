# Building and testing

An operating system is more than its kernel: it needs a compiler that
produces programs for it, a C library that speaks its system calls, the
programs themselves, a root filesystem to put them in, and something to
boot it all from. AquilaOS builds every piece from source, in one tree,
with one `make`: the cross toolchain, the C library, the kernel, the base
system's programs, the third-party software, the RAM disk and the disk
images. Then it boots the result in QEMU and checks it.

This document describes the build, and the tests. Most of it is the same
for every architecture; what isn't is a configuration file and a few
names, listed at the end (*What depends on the architecture*).

## The pieces

| Piece                    | From                     | Built by                    | Into (x86_64)                    |
|--------------------------|--------------------------|-----------------------------|----------------------------------|
| the cross toolchain      | binutils, gcc, `toolchain/patches/` | `make toolchain`   | `build/toolchain/`               |
| the C library (newlib)   | newlib, `libc/aquila/`   | `make toolchain`            | `build/sysroot-x86_64/`          |
| the kernel               | `kernel/`                | `make kernel`               | `build/x86_64/kernel/kernel-0.0.1.x86_64` |
| the base programs        | `userland/aqbox/`        | `make userland`             | `build/x86_64/userland/bin/`     |
| the base third-party software | `contrib/` recipes  | `make contrib`              | `build/x86_64/contrib/root/`     |
| optional software        | `ports/` recipes         | `make ports`                | `build/x86_64/ports/root/`       |
| the root filesystem, the RAM disk | all of the above, `rootfs/` | `make initrd`   | `build/x86_64/rootfs/`, `initrd.img` |
| the images               | the kernel, the RAM disk, Limine | `make iso`, `hdd`, `live` | `build/x86_64/aquila.iso`, `aquila.img`, `live.img` |

`make` alone builds the kernel, the base programs, contrib and the RAM
disk; the toolchain must be there first (`make toolchain`, once). The
targets are listed at the top of the [Makefile](../../Makefile).

Everything goes in `build/`. The x86_64 system is in `build/x86_64/`, the
aarch64 one in `build/aarch64/`, the i386 one in `build/` itself; the
toolchains of every architecture, and the downloaded sources, are shared:
`build/toolchain/`, `build/downloads/`.

## The toolchain

Programs for AquilaOS are built with a *cross compiler*: a gcc that runs
on the host and produces AquilaOS's programs. Its target is
`x86_64-aquila` (`i686-aquila` for i386, `aarch64-aquila` for aarch64),
a system gcc and binutils know nothing about until they're patched
(`toolchain/patches/`): the patches add the target, which is ELF, with
newlib, and AquilaOS's defaults: the C library's start files (`crt0.o`,
`crti.o`), and static linking, the only kind there is.

[`toolchain/build.sh`](../../toolchain/build.sh) builds it, in stages,
which can also be run alone (`toolchain/build.sh headers libc`):

| Stage       | What                                                                    |
|-------------|-------------------------------------------------------------------------|
| `fetch`     | the source archives, checked against their SHA-256                      |
| `autotools` | the exact autoconf and automake versions newlib needs                   |
| `binutils`  | the assembler, the linker                                               |
| `gcc`       | the C compiler, and `libgcc`                                            |
| `headers`   | `include/uapi/` into the sysroot's `/usr/include`                       |
| `libc`      | newlib, with AquilaOS's port, into the sysroot                          |
| `limine`    | the Limine boot loader and its host tool, for the images (x86 only)     |

The *sysroot* (`build/sysroot-<arch>/`, `build/sysroot/` for i386) is the
target system's `/usr/include` and `/usr/lib` as the cross compiler sees
them.

### The kernel's ABI headers

`include/uapi/aquila/` holds what the kernel and the C library must agree
on: the system call numbers and their argument structures, `errno`
values, signals, `struct stat`, `ioctl` requests, and so on (see
[System calls](syscalls.md)). The kernel is compiled with it on its
include path; the `headers` stage installs it into the sysroot, where
the C library and programs find it as `<aquila/...>`. After newlib is
built, `libc/aquila/abi-check.c` is compiled with `-Werror` against both
newlib's headers and the kernel's: a value that differs (an `errno`, a
signal number, a field's offset) stops the build.

### The C library

The C library is newlib, a small C library made to be ported. Its port to
AquilaOS is in [`libc/aquila/`](../../libc/aquila/), copied into
newlib's tree (`newlib/libc/sys/aquila`) when it's built: the start-up
code (`crt0.c`, `crti.S`, `crtn.S`), the system call wrappers
(`syscalls.c`, over the traps in `syscall.h`), and what newlib doesn't
have (`pthread.c`, `dirent.c`, `auxv.c` for `getauxval()`, the
resolver, `termios.c`, the password and group files, ...). Its sources are listed in `Makefile.inc`.

## The kernel

### A non-recursive build

[`kernel/Makefile`](../../kernel/Makefile) is one make process for the
whole kernel. It walks a tree of `Build.mk` files, starting with the
architecture's (`kernel/arch/<arch>/Build.mk`), then `core/`, `fs/`,
`dev/`, `sys/`, `mm/` and `net/`. Each lists its objects and its
subdirectories:

```make
obj-y  += vfs.o                 # always
obj-$(FS_EXT2) += ext2.o        # if the configuration says FS_EXT2=y
dirs-y += posix/
dirs-$(FS_EXT2) += ext2/
```

Every object is compiled into `build/<arch>/kernel/` (`build/kernel/`
for i386), with dependencies from the compiler (`-MMD -MP`), and all of
them are linked with the architecture's linker script
(`kernel/arch/<arch>/kernel.<arch>.ld`). The link order is the order of
the walk, and it matters: the built-in modules (drivers, filesystems)
are initialized in link order (see [Booting](booting.md)).

The include path puts `include/uapi` first, then the platform's headers,
the architecture's, and the kernel's; that's how the architecture
completes the kernel's headers (see [The kernel and the
machine](architecture.md)).

### Configurations

A configuration, `configs/<name>.mk`, says which compiler, which flags,
which architecture and platform, and which drivers and filesystems:

```make
ARCH=x86_64
ARCH_DIR=x86_64
TARGET=x86_64-aquila
ARCH_BITS=64
PLATFORM_X86_PC=y
PLATFORM_DIR=pc
PLATFORM_X86_MISC_APIC=y
DEV_ATA=y
DEV_E1000=y
FS_EXT2=y
NET_INET=y
...
```

`CONFIG=` chooses one, for the kernel and for everything else (the
toolchain's target, the build directory): `x86_64-pc` by default,
`i386-pc` for i386, `aarch64-virt` for aarch64.

The configuration's variables choose what is compiled, and its options
set to `y` reach C too, as `CONFIG_<option>` macros (`CONFIG_DEV_PCI`),
for the code that only some configurations have (`#ifdef
CONFIG_DEV_CONSOLE`). The values C needs come from `config.h`
(`kernel/include/config.h`, completed by the architecture's): the
architecture's word size, `uname`'s strings, limits such as the number of
descriptors a process may have.

The kernel can be built with the host's gcc too, with no cross toolchain:
`tests/host/x86_64-pc.mk` and `tests/host/i386-pc.mk` are the same
configurations with `gcc` and `ld`, and the flags that turn off what
distributions' compilers do by default (position-independent code, the
stack protector, CET). The tests use them, and so does `make run-min`
(below). aarch64's configuration builds the kernel with clang already,
which needs no toolchain of its own either.

On x86 the kernel also builds with tcc, the Tiny C Compiler:
`tests/host/x86_64-pc-tcc.mk` and `tests/host/i386-pc-tcc.mk` compile
the C with it (`TCC=`, by default `tcc`, and for i386 `i386-tcc`, its
i386 compiler), and assemble and link with the host's gcc and `ld`:
tcc's assembler doesn't take all of the kernel's assembly, and its
linker doesn't take linker scripts. The kernel is linked with tcc's
runtime library (`libtcc1.a`, as gcc's would be `libgcc`), which has
what tcc calls instead of generating it: atomic operations, 64-bit
shifts on i386, `va_arg` on x86_64. Being built by two compilers keeps
the kernel's C plain; what it still needs from one of them is in
`core/system.h`, and what a compiler may call by itself (`memcpy()`, a
`va_arg` that fails) in `core/string.c` and `core/compiler.c`.

`DEBUG=1` adds debugging information (`-g`) and lowers the optimization
to `-Og`, which keeps the code close to the source for gdb. The linker
scripts keep the DWARF sections in the file, not loaded. Flags alone
don't make objects be rebuilt, so a debug build goes into a directory
of its own (`make debug` uses `build/min-<arch>-debug/`).

## User space

### The base programs

[`userland/Makefile`](../../userland/Makefile) builds the programs in
`userland/`, each from every `.c` file in its directory, with the cross
compiler. There is one, **aqbox**, a multi-call program in the manner of
BusyBox: one binary, linked as `getty`, `login`, `mount`, `ps`,
`ifconfig`, `ping`, `route`, `reboot`, `poweroff`, ..., which runs the
applet its name says. These are the programs that know AquilaOS's own
interfaces (its `/proc`, its network `ioctl`s, its MBR reader). They're
rebuilt when the C library changes.

### Third-party software: contrib and ports

The rest is third-party software, built from upstream sources by
*recipes*, as the BSDs' ports do:

- [`contrib/`](../../contrib/README.md) is the base system's: dash
  (`/bin/sh`), sbase (the core utilities), runit (init and service
  supervision), netcat, sdhcp. `make` builds it, and every RAM disk has
  it.
- [`ports/`](../../ports/README.md) is optional: bash, curl, Lua, tcc,
  ncurses, an X server (TinyX) with its libraries, twm, st, dwm, ... `make
  ports` builds them, and the RAM disk includes them once built.

A recipe, `<tree>/<name>/recipe.sh`, gives the release (`NAME`,
`VERSION`, `URL` and `SHA256`, or a git `COMMIT`), its dependencies, the
architectures it builds for, and a `build()` function; patches next to it
are applied first. [`mk/recipes.sh`](../../mk/recipes.sh) downloads,
checks, unpacks and builds it, with the cross compiler, and installs the
programs into `build/<arch>/<tree>/root/` and the libraries other recipes
need into `build/<arch>/staging/`. `make ports` builds those out of date
(their recipe, the C library or a dependency changed since),
`mk/recipes.sh ports lua` builds one anyway.

## The root filesystem and the images

### The RAM disk

`make initrd` puts together the root filesystem, `build/<arch>/rootfs/`:

1. the directories (`bin`, `sbin`, `dev`, `proc`, `tmp`, ...) and the
   files of [`rootfs/`](../../rootfs/) (`/etc`: accounts, services);
   `ROOTFS_OVERLAY=<dir>` copies another tree over them;
2. `/init`, a link to runit's `/sbin/runit-init`;
3. aqbox and its links;
4. contrib's root, then the ports' if they were built;
5. every program stripped.

Then it makes it a `cpio` archive, `initrd.img`, in the old binary
format (`cpio -H bin`), which is what the kernel's initramfs reads (see
[Filesystems](filesystems.md)).

### Images

| Target           | Image                    | What                                             |
|------------------|--------------------------|--------------------------------------------------|
| `make iso`       | `aquila.iso`             | a CD: Limine, the kernel, the RAM disk (`boot/limine.conf`) |
| `make hdd`       | `aquila.img`             | a hard disk: Limine on a FAT32 `/boot`, the root filesystem on ext2, the second partition (`mk/hdd.sh`) |
| `make hdd-kernel`| `aquila.img`             | the kernel put on the existing disk (`mk/hdd-kernel.sh`) |
| `make hdd-update`| `aquila.img`             | the kernel and the system just built put on it; home directories and accounts kept (`mk/hdd-update.sh`) |
| `make live`      | `live.img`               | i386 only: a 2 MiB disk with the kernel and the base system's RAM disk, gzipped, for v86 in a browser (`mk/live.sh`) |

The disk images are made without root privileges: the partitions are
written as files (`mkfs.fat` with mtools for FAT, `mke2fs -d` under
`fakeroot` for ext2) and put together with `sfdisk`; Limine's tool
installs its boot code. See [Booting](booting.md) for how they boot.
Limine is a PC boot loader, so the images are x86's only; aarch64's
kernel is booted by QEMU itself (`make run`).

### Running

| Target         | What                                                                 |
|----------------|----------------------------------------------------------------------|
| `make run`     | QEMU loads the kernel and the RAM disk itself (`-kernel`, `-initrd`), the console on the terminal |
| `make run-iso` | boots the CD, on QEMU's screen                                       |
| `make run-hdd` | boots the hard disk, after putting the system just built on it      |
| `make run-min` | no toolchain: the kernel built with the host's gcc (aarch64's with clang), and [`start/init.c`](../../start/init.c), a tiny shell without a C library, as the only program (`build/min-<arch>/`) |
| `make debug`   | `run-min`'s, built with `DEBUG=1`, QEMU stopped before the first instruction with its gdb server on port 1234 (`-s -S`) |

QEMU uses KVM when `/dev/kvm` can be used; `KVM=0` turns it off.
`make min` builds `run-min`'s kernel and RAM disk without running them.
`make clean` removes what was built but the toolchain, the downloads and
the hard disk; `make distclean` removes the architecture's build
directory (`build/x86_64/`; for i386, all of `build/`).

### Debugging

`make debug` prints the command for gdb: `gdb -x mk/gdbinit` and the
kernel's ELF file. [`mk/gdbinit`](../../mk/gdbinit) connects to QEMU and
stops at `kmain()` (a hardware breakpoint: the kernel's addresses aren't
mapped when QEMU starts); from there `break sys_fork`, `continue`, `bt`,
`next`, `print *curthread` work as in any program. aarch64's kernel is
debugged the same way (`make debug CONFIG=aarch64-virt`).

A panic prints a stack trace: the return addresses on the kernel stack,
found through the frame pointers (every configuration keeps them). The
function names come from the kernel's symbol table if the boot loader
passed it (Limine does, QEMU's `-kernel` doesn't).
[`mk/backtrace.sh`](../../mk/backtrace.sh) names the addresses from the
kernel's file instead, with the file and line of each in a `DEBUG=1`
build:

```
mk/backtrace.sh build/min-x86_64-debug/kernel/kernel-0.0.1.x86_64 < serial.log
```

`kvmem.guard` on the kernel's command line makes one on purpose (a write
past the end of an object: see [Memory](memory.md)).

## The documentation

`make docs` builds these documents and the code reference, generated by
Doxygen from the comments in the sources, into `build/docs/kernel/api/`
(`docs/kernel/Doxyfile`); `make docs-serve` serves them on
<http://localhost:8000/>. A push to `main` that changes them, or the
sources, publishes them on GitHub Pages
([`.github/workflows/docs.yml`](../../.github/workflows/docs.yml), with
Doxygen 1.18.0, the release they build without a warning with).

## Testing

The kernel is tested by booting it. [`tests/run.sh`](../../tests/run.sh)
builds the kernel with the host's gcc (the configurations in
`tests/host/`, into `tests/out/x86_64/`, or `tests/out/` for i386; for
aarch64, with clang, `configs/aarch64-virt.mk`, into
`tests/out/aarch64/`), then, for each test, boots it in QEMU with a RAM
disk whose `/init` is the test's program, and reads what it reports on
the serial port.

### Test programs

Most tests are a single C file, `tests/<name>/init.c`, compiled with the
host's compiler as a static, freestanding program: no C library, the
system calls made directly (`int $0x80`, aarch64's `svc #0`) through
[`tests/lib/aqtest.h`](../../tests/lib/aqtest.h), and the kernel's ABI
headers (`include/uapi`) for the numbers and structures. A program needs
nothing more to run on AquilaOS. Each check prints a line:

```
PASS getpid got=1 want=1
FAIL read past the end got=-14 want=0
aqtest: done, fails=1
```

A test fails on a `FAIL` line, a kernel panic, a reset of the machine
(QEMU runs with `-no-reboot`, so a triple fault ends it), or if it
doesn't finish in time. Some tests have a partner on the host: a Python
program playing the other end of a network (`tests/net/peer.py`), a
clock (`tests/clock/stamp.py`), an HTTPS server (`tests/curl/server.py`).

Other tests boot the real system: the `libc` test's program is built with
the cross compiler against newlib; `system` boots the RAM disk `make`
built and logs in on the serial console; `disk` installs it on a hard
disk image and boots that; `tcc`, `curl` and `bash` run those ports.
They're skipped when what they need wasn't built.

### The tests

| Test       | What it checks                                                         |
|------------|------------------------------------------------------------------------|
| `ata`      | the disk on the first ATA channel is found, over several boots         |
| `cpu`      | the kernel's entry (flags and segments sanitized, stack alignment), user exceptions to signals, port I/O refused, SSE |
| `syscalls` | the system calls without libc: bad pointers (`EFAULT`), descriptors, pipes, signals and handlers, process groups, `fork` and copy on write, threads, path lookup, links, permissions, sleeping |
| `memory`   | more RAM than 4 GiB (or than i386's direct map) filled and read back   |
| `oom`      | out of memory (128 MiB): a process with none left is killed, the system goes on and has its memory back |
| `kvmem`    | the kernel's allocator: its self-test, no leaks after forks and execs, guard pages |
| `clock`    | the clock from each source (TSC, HPET, PIT), against the host's time  |
| `irq`      | interrupts through the APIC and the 8259, `/proc/interrupts`, the timer's rate, MSI |
| `smp`      | four CPUs: threads at once, TLB shootdown, `fork`, `exec` and kills across CPUs, `smp=off` |
| `net`      | the e1000 (line and MSI), ARP, ICMP, UDP and TCP against a peer on the host, segment by segment |
| `tcp`      | megabytes of TCP against QEMU's user network                           |
| `ext2`     | writing an ext2 image (indirect blocks, renames, truncation), then `e2fsck` on the host |
| `minix`    | the same on a minix image, `fsck.minix`                               |
| `libc`     | newlib on AquilaOS: stdio, directories, accounts, sockets, the resolver |
| `system`   | the RAM disk `make` built: logins, dash, pipelines, `nc`, DHCP         |
| `disk`     | the system on a hard disk image, booted twice with Limine, `e2fsck`    |
| `tcc`      | the tcc port compiles and runs programs                                |
| `curl`     | the curl port over TLS against a server on the host                   |
| `bash`     | the bash port runs a script of its features                            |
| `aarch64`  | aarch64 only: user mode, system calls, `fork`, `exec`, pipes, memory, faults, signals, the tick, an e1000 on PCI Express |

[`tests/README.md`](../../tests/README.md) has the details of each, and
how to write one.

### Options

```
tests/run.sh [-a arch] [-C cc] [-c cpus] [-j jobs] [-k args] [-n] [-v] [test...]
```

| Option | What                                                                         |
|--------|------------------------------------------------------------------------------|
| `-a`   | the architecture: `x86_64` (default), `i386`, or `aarch64` (QEMU's virt machine, built with clang: the `aarch64` and `oom` tests) |
| `-C`   | the kernel's compiler, on x86: `gcc` (default) or `tcc` (above), built into `tests/out/<arch>-tcc/` |
| `-c`   | the number of CPUs QEMU has, for every boot (but the `irq` and `smp` tests, which choose) |
| `-j`   | the tests run at once (default 1); those that time things or take much memory (`clock`, `irq`, `smp`, `memory`) run alone, after |
| `-k`   | more for the kernel's command line, for every boot (e.g. `apic=off`)          |
| `-n`   | don't build the kernel again, use the last one                                |
| `-v`   | print the serial log of the tests that failed                                 |

With test names, only those run. `make test` is `tests/run.sh -a $(ARCH)`.

A change to the kernel is checked on the whole matrix:

```
tests/run.sh                    # x86_64
tests/run.sh -a i386            # i386
tests/run.sh -k apic=off        # the 8259 and the PIT, one CPU
tests/run.sh -n -c 4            # four CPUs, every test
tests/run.sh -n -a i386 -c 4    # the same on i386
tests/run.sh -a aarch64         # aarch64
tests/run.sh -C tcc             # x86_64, the kernel built with tcc
tests/run.sh -a i386 -C tcc     # the same on i386
```

`-j 4` (to any of them) runs 4 tests at once.

### CI

[`.github/workflows/build.yml`](../../.github/workflows/build.yml)
builds everything on every push and pull request: the toolchain (cached,
rebuilt when `toolchain/`, `libc/` or `include/uapi/` change), the base
system and the ports, the images; runs `tests/run.sh -v` (x86_64, with
KVM); builds the documentation, with Ubuntu's Doxygen, and fails on any
Doxygen warning; and keeps the images and the documentation as the run's
artifacts.

## What depends on the architecture

Almost nothing in the build knows about x86; the architecture comes in
through the configuration:

| What                  | x86_64                         | i386                          | aarch64                       |
|-----------------------|--------------------------------|-------------------------------|-------------------------------|
| configuration         | `configs/x86_64-pc.mk` (default) | `configs/i386-pc.mk`        | `configs/aarch64-virt.mk`     |
| the kernel's sources  | `kernel/arch/x86_64/` (a link to `i386/`) | `kernel/arch/i386/`   | `kernel/arch/aarch64/`        |
| the kernel's compiler | the toolchain's gcc            | the toolchain's gcc           | clang and ld.lld              |
| toolchain target      | `x86_64-aquila`                | `i686-aquila`                 | `aarch64-aquila`              |
| sysroot               | `build/sysroot-x86_64/`        | `build/sysroot/`              | `build/sysroot-aarch64/`      |
| build directory       | `build/x86_64/`                | `build/`                      | `build/aarch64/`              |
| QEMU                  | `qemu-system-x86_64`           | `qemu-system-i386`            | `qemu-system-aarch64` (virt)  |

The toolchain script and the recipes take `ARCH=x86_64`, `ARCH=i386` or
`ARCH=aarch64` (make passes its configuration's); a recipe can say which
architectures it builds for (`ARCHS`, Microwindows' is x86's only).
The images are x86's only (Limine isn't built for aarch64), and the live
image is i386's only, made for v86, which emulates a 32-bit PC.

### x86: compiler flags

The kernel's compiler flags are in its configuration:

- **x86_64**: `-mcmodel=kernel`, since the kernel is linked in the top
  2 GiB of the address space (see [Booting](booting.md)); `-mno-red-zone`,
  because an interrupt pushes onto the kernel stack where a function
  could have kept data below the stack pointer; `-mgeneral-regs-only`, no
  SSE in the kernel, whose registers aren't saved on entry (the FPU is
  switched lazily, see [Interrupts](interrupts.md));
- **i386**: `-march=i686`.
- Both: `-ffreestanding -nostdlib`, no C library, and `-Werror`.

## Where it is

| File                               | Layer    | What                                    |
|------------------------------------|----------|-----------------------------------------|
| `Makefile`                         | common   | every target                            |
| `kernel/Makefile`, `kernel/**/Build.mk` | common | the kernel's build                  |
| `configs/`                         | common   | the configurations (`gcc.mk`: i386's compiler) |
| `kernel/arch/i386/kernel.*.ld`     | x86      | the linker scripts                      |
| `kernel/arch/aarch64/kernel.aarch64.ld` | aarch64 | the linker script                   |
| `toolchain/build.sh`, `toolchain/patches/` | common | the cross toolchain and newlib  |
| `include/uapi/`                    | common   | the kernel's ABI headers                |
| `libc/aquila/`                     | common   | newlib's port, `abi-check.c`            |
| `userland/`                        | common   | aqbox                                   |
| `contrib/`, `ports/`, `mk/recipes.sh` | common | third-party software                  |
| `rootfs/`                          | common   | the root filesystem's own files         |
| `boot/`, `mk/hdd*.sh`, `mk/live.sh` | PC      | the boot loader's configurations, the images |
| `start/init.c`                     | common   | `make run-min`'s program               |
| `mk/gdbinit`, `mk/backtrace.sh`    | common   | debugging                               |
| `tests/run.sh`, `tests/*/`, `tests/lib/aqtest.h` | common | the tests                      |
| `tests/host/`                      | x86/PC   | the configurations for the host's gcc, and for tcc |
| `docs/kernel/`                     | common   | these documents, `Doxyfile`             |
| `.github/workflows/build.yml`      | common   | CI                                      |
| `.github/workflows/docs.yml`       | common   | the documentation on GitHub Pages       |

## References

- *GCC Cross-Compiler* and *OS Specific Toolchain*, the OSDev wiki: why a
  cross compiler, and how a new target is added to binutils and gcc.
- *Porting newlib*, the newlib documentation (`libc/sys/` and the system
  call stubs it expects).
- *Recursive Make Considered Harmful*, Peter Miller (1997): the argument
  for one make process over a whole tree.
- The FreeBSD Handbook, *Installing Applications: Packages and Ports*:
  recipes that build upstream software for a system.
- QEMU's documentation: `-kernel`/`-initrd` (Multiboot), `-netdev
  socket`, `-smp`.
