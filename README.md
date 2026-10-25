# Aquila OS [![build](https://github.com/thethumbler/aquila/actions/workflows/build.yml/badge.svg)](https://github.com/thethumbler/aquila/actions/workflows/build.yml)

A small UNIX-like operating system: its kernel, C library and system
tools. It's meant to be *boring*: the kernel does what UNIX kernels have
done for decades, the plain way, in code small enough to read in full,
with documents that explain each part of it. It's for people who want
to understand operating systems by reading and changing one, and
especially for those writing their own. It runs on x86 (i386 and
x86-64) and on aarch64 (QEMU's virt machine).

## Start here

You need `gcc`, `ld`, `cpio` and `qemu-system-x86_64`, nothing else:

```
make run-min        # the kernel, booted with one tiny program (a minute)
```

builds the kernel with your gcc and boots it in QEMU with
[start/init.c](start/init.c) as the only program: a tiny shell with no
C library, each of its commands a system call or two. Type `help`;
Ctrl-A X quits QEMU. Then:

1. **Read** [docs/kernel/](docs/kernel/README.md), in order: how the
   kernel boots, takes interrupts, manages memory and processes, and the
   rest, each with pointers into the code. [kernel/](kernel/README.md)
   lists what's where.
2. **Change something.** Add a command to `start/init.c`, or a
   `printk()` to a system call (`sys_getpid()` in
   `kernel/sys/sys_proc.c`, the others in `kernel/sys/sys_*.c` by
   subject), and `make run-min` again.
3. **Debug it.** `make debug` boots the same with debugging information
   and QEMU waiting for gdb: `gdb -x mk/gdbinit` and the kernel file it
   names stops in `kmain()`. A panic prints a stack trace;
   [mk/backtrace.sh](mk/backtrace.sh) names its addresses
   (`mk/backtrace.sh <kernel> < log`).
4. **Test it.** `make test` runs the kernel's test suite, built with your
   gcc too ([tests/](tests/README.md)).
5. **The whole system**: a cross toolchain (`make toolchain`, about 15
   minutes), then `make run` boots it with a shell, the core utilities
   and logins, as below.

For aarch64, `make run-min CONFIG=aarch64-virt` (needs `clang`, `ld.lld`,
`llvm-objcopy` and `qemu-system-aarch64`); for i386, `CONFIG=i386-pc`
(gcc with 32-bit support, and `qemu-system-i386`).

## Building

Everything is built from source into `build/` (the x86-64 system into
`build/x86_64`). On a Linux host you need a C/C++ compiler with 32-bit
support, `make`, `bison`, `flex`, `texinfo`, GMP, MPFR and MPC
development headers, `wget`, `git`, `nasm`, `cpio`, `xorriso` and
`qemu-system-x86_64` (and `gperf`, autoconf, automake, libtool,
`pkg-config` and `python3` for the ports). On Debian/Ubuntu:

```
sudo apt install build-essential gcc-multilib bison flex texinfo libgmp-dev \
    libmpfr-dev libmpc-dev wget git nasm cpio xorriso qemu-system-x86 gperf \
    autoconf automake libtool pkg-config python3
```

Then:

```
make toolchain      # x86_64-aquila gcc/binutils, newlib and Limine (once, ~15 min)
make                # kernel, user space programs, contrib/ and initrd
make ports          # optional software (vis, kilo, Lua, ...), included in the initrd
make run-iso        # build the ISO and boot it in QEMU
```

As on the BSDs, some third-party software is part of the base system:
[contrib/](contrib/) (dash as `/bin/sh`, sbase for the core utilities,
runit as init) is fetched from upstream and built by `make`. [ports/](ports/) is
optional software, built with `make ports`.

`make run` boots the kernel directly with the console on the serial port
(log in as `root`, no password). On the screen (`make run-iso`), the
kernel's virtual terminals have logins on `/dev/tty1` to `/dev/tty3`,
Alt+F1 to Alt+F6 switch between them. runit supervises the logins as
services (`/etc/sv`, enabled in `/etc/service`, see `sv`); `poweroff` and
`reboot` shut down through it, as does Ctrl+Alt+Del. `make iso` only builds
`build/x86_64/aquila.iso`. With the ports (`make ports`), `startx` on the screen
starts X: twm and an xterm (Ctrl+Alt+F1 to Ctrl+Alt+F6 switch terminals
from it).

The ISO and `make run` boot the ramdisk, read-only and the same on every
boot. `make hdd` makes `build/x86_64/aquila.img`, a hard disk with AquilaOS
installed: Limine on a FAT32 `/boot`, the root filesystem on ext2
(`root=/dev/hda2`), where what is written is kept. `make run-hdd` boots
it (making it the first time) with the system just built: `make
hdd-update` puts the kernel and the system's files on it and keeps the
rest (home directories, and the accounts in `/etc`), `make hdd-kernel`
only the kernel, `make hdd` makes the whole disk anew. See the top of the [Makefile](Makefile)
for all targets.

### Network

The kernel drives Intel 8254x Ethernet cards (e1000), QEMU's default;
`lo`, the loopback interface (127.0.0.1), is always there. IPv4 is
configured with the kernel argument `ip=<address>/<prefix
length>[:<gateway>]`, e.g. `ip=10.0.2.15/24:10.0.2.2` for QEMU's user
network, or on the running system:

```
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
route add default gw 10.0.2.2
ping 10.0.2.2
```

or from a DHCP server (sdhcp, contrib): `ifconfig eth0 up; sdhcp eth0`,
which sets the address, the default route and `/etc/resolv.conf`, and
renews the lease. On a disk, the `dhcp` service does it at every boot
once enabled: `ln -s /etc/sv/dhcp /etc/service/`.

Programs have TCP (`SOCK_STREAM`), UDP and ping sockets (`SOCK_DGRAM`,
`IPPROTO_ICMP`). Host names are looked up in `/etc/hosts`, then with
the DNS servers `/etc/resolv.conf` names (DHCP's), e.g.
`nc example.com 80`.

### i386

The default is x86-64. AquilaOS also runs on i386 (using at most
768 MiB of memory, what its kernel maps), which is built into
`build/` with an i686-aquila toolchain (next to the x86_64 one in
`build/toolchain`), `CONFIG=i386-pc` selects it:

```
make toolchain CONFIG=i386-pc       # i686-aquila gcc/binutils, newlib and Limine
make CONFIG=i386-pc                 # build/
make hdd CONFIG=i386-pc             # build/aquila.img
make test CONFIG=i386-pc            # or tests/run.sh -a i386
```

### aarch64

AquilaOS also runs on QEMU's virt machine (aarch64), one CPU, built into
`build/aarch64` with an aarch64-aquila toolchain (also in
`build/toolchain`); the kernel is built with `clang` and `ld.lld`.
`CONFIG=aarch64-virt` selects it:

```
make toolchain CONFIG=aarch64-virt  # aarch64-aquila gcc/binutils and newlib
make CONFIG=aarch64-virt            # build/aarch64/
make ports CONFIG=aarch64-virt      # stops at TinyX, which doesn't build there yet
make run CONFIG=aarch64-virt        # console on stdio (the PL011, /dev/ttyS0)
make test CONFIG=aarch64-virt       # or tests/run.sh -a aarch64
```

There are no virtual terminals (no framebuffer), so the logins are on the
serial console only, and the graphical ports (TinyX, Microwindows) have
nothing to draw on; `ARCH=aarch64 mk/recipes.sh ports <name>` builds
the others one by one. `make iso`, `make hdd` and the targets that use
them are x86's: they boot with Limine, a PC boot loader, and the kernel
has no disk driver on aarch64. `make live` is i386's.

## Testing

```
make test           # or tests/run.sh
```

boots the kernel in QEMU with test programs and checks the system built
from source, see [tests/](tests/README.md).

## Documentation

How the kernel works, subsystem by subsystem, is in
[docs/kernel/](docs/kernel/README.md). Its code reference, from the
comments in the sources:

```
make docs           # build/docs/kernel/api/index.html (needs doxygen)
make docs-serve     # the same, on http://localhost:8000/
```

They are also published on GitHub Pages from `main`, by
[.github/workflows/docs.yml](.github/workflows/docs.yml).

## Layout

| directory                    |                                                    |
|------------------------------|----------------------------------------------------|
| [start/](start/)             | the first program to read: `make run-min`'s /init  |
| [kernel/](kernel/)           | the kernel                                         |
| [include/uapi/](include/uapi/aquila) | user/kernel ABI: syscall numbers, errno, structures shared by the kernel and libc |
| [libc/](libc/aquila)         | AquilaOS port of newlib                            |
| [toolchain/](toolchain/)     | cross toolchain build script and patches           |
| [userland/](userland/)       | aqbox (system tools: login, ps, mount, ...)        |
| [contrib/](contrib/)         | third-party software of the base system (recipes)  |
| [ports/](ports/)             | optional third-party software (recipes)            |
| [mk/](mk/)                   | build scripts (contrib/ and ports/ recipes, disks), gdb and backtrace helpers |
| [rootfs/](rootfs/)           | root filesystem skeleton                           |
| [boot/](boot/)               | bootloader configuration                           |
| [configs/](configs/)         | kernel build configurations                        |
| [tests/](tests/)             | QEMU based test suite                              |
| [docs/](docs/)               | documentation: the kernel's design, its code reference's configuration |

## Features

#### Architectures
- [X] x86: i386 and x86-64 (PCs: QEMU; v86 in a browser, i386)
- [X] aarch64 (QEMU's virt machine: one CPU)

- [X] Multitasking
- [X] Multithreading
- [X] SMP     (up to 32 CPUs from ACPI's MADT, a kernel lock: user mode
              runs on every CPU at once, the kernel on one at a time;
              `smp=off`)

#### Kernel
- [X] Monolithic kernel
- [X] A clock to the nanosecond: the TSC (invariant) or the HPET, the
      PIT's ticks without them (`clock=tsc|hpet|pit`)
- [X] Interrupts through the APIC (I/O APIC routes from ACPI's MADT, the
      local APIC's timer as the tick, shared IRQs, MSI), the 8259 and the
      PIT without one (`apic=off`); `/proc/interrupts`
- [X] Kernel threads (process 0, `[kernel]`; `ps` shows them as `[net]`, ...)
- [X] POSIX signals, delivered on the way back to user mode (a handler's
      frame keeps the FPU state, `sigreturn` from a kernel-mapped page)
- [X] Virtual Filesystem
- [ ] Loadable modules

#### Filesystems
- [X] initramfs (CPIO archive, read only)
- [X] tmpfs     (temporary filesystem, read/write)
- [X] devfs     (device files, read/write)
- [X] devpts    (pseudo-terminals, read/write)
- [X] procfs    (process information, read only)
- [X] ext2      (read/write)
- [X] minix     (read/write)
- [ ] ext3
- [ ] ext4

#### Devices
- [X] i8042   (PS/2 controller)
- [X] ps2kbd  (PS/2 keyboard)
- [X] ps2mouse (PS/2 mouse, /dev/mouse as Linux's /dev/psaux)
- [X] ramdisk (the initrd as a block device)
- [X] memdev  (/dev/null, /dev/zero, /dev/random, /dev/urandom, /dev/kmsg)
- [X] vt      (virtual terminals on the framebuffer or VGA text mode;
              graphics, raw keyboard and switching as the Linux console)
- [X] pty     (pseudo-terminals: /dev/ptmx and /dev/pts)
- [X] ata     (ATA hard disks, bus master DMA, or PIO with `ata.nodma`)
- [X] fbdev   (framebuffer, VESA; the Linux framebuffer API)
- [X] 8250    (UART: COM1, COM2)
- [X] pl011   (ARM's UART, aarch64)
- [X] pci     (configuration mechanism #1, and ECAM on aarch64)
- [X] rtc     (the date: the PC's CMOS clock, the PL031 on aarch64)
- [X] e1000   (Intel 8254x Ethernet, and the 82574L with MSI: QEMU's
              e1000 and e1000e; on aarch64 too, over PCI Express)

#### Network
- [X] IPv4, ARP, ICMP (echo, unreachable), the loopback interface (lo)
- [X] UDP and ping sockets
- [X] TCP     (RFC 9293: retransmission, NewReno congestion control,
              delayed ACKs, Nagle, keepalive; no window scaling, SACK or
              timestamps yet)
- [X] DHCP    (sdhcp, contrib)
- [X] DNS     (the C library's resolver: /etc/hosts, then DNS over UDP,
              TCP for long answers)

#### System
- [X] newlib 4.6 C library
- [X] dash    (shell, contrib)
- [X] sbase   (core utilities, contrib)
- [X] netcat  (OpenBSD's nc, contrib)
- [X] runit   (init and service supervision, contrib)
- [X] aqbox   (system tools)
- [X] kilo    (text editor, port)
- [X] vis     (vi-like editor, port) on ncurses (port)
- [X] bash    (shell, port)
- [X] Lua     (port)
- [X] tcc     (C compiler, port), building programs on AquilaOS
- [X] curl    (port): HTTPS through Mbed TLS (port), with Mozilla's CA
              certificates (port)
- [X] TinyX   (Xfbdev, X server on the framebuffer, port) with X.Org's
              fixed and cursor fonts, Xlib and the X Toolkit (ports)
- [X] twm and xterm (ports), `startx`
- [X] dwm, st and dmenu (ports), with Xft, fontconfig, FreeType and
              DejaVu fonts (ports)
- [X] Doom    (doomgeneric with Freedoom, ports): on the framebuffer and
              in an X window
- [X] Microwindows (Nano-X, port, x86 only): windows on the framebuffer,
              its demos, Nuklear's, nxchess and nxscribble among them,
              and nxterm
- [X] UnixBench (port): `ubench`, its index without Perl

## License

AquilaOS is dedicated to the public domain under
[CC0 1.0 Universal](LICENSE): you can copy, modify and distribute it,
even commercially, without asking permission.

Third-party code keeps its own license:

| component | location | license |
|-----------|----------|---------|
| unscii-16 font | [kernel/arch/i386/earlycon/font.h](kernel/arch/i386/earlycon/font.h) | public domain, Viznut |
| multiboot.h | [kernel/arch/i386/include/boot/multiboot.h](kernel/arch/i386/include/boot/multiboot.h) | permissive, Free Software Foundation |
| doxygen-awesome-css | [docs/kernel/doxygen/](docs/kernel/doxygen/) | MIT, jothepro |
| IBM Plex Sans, Martian Mono (fonts) | [docs/kernel/doxygen/fonts/](docs/kernel/doxygen/fonts/) | SIL Open Font License 1.1 |

The toolchain patches in [toolchain/patches/](toolchain/patches/) modify
binutils and GCC (GPLv3) and newlib (various free licenses); the patched
tools remain under those licenses. contrib/ and ports/ software is
fetched from upstream and keeps its own license, among them: dash,
netcat, runit and Freedoom: BSD; kilo: BSD 2-Clause; sbase, sdhcp, Lua,
libtermkey, expat, dwm, st and dmenu: MIT; ncurses, xterm, curl,
fontconfig and the X.Org libraries: MIT-style; X.Org's misc and cursor
fonts: public domain; vis: ISC; zlib: zlib; FreeType: FreeType License
or GPLv2; DejaVu fonts: Bitstream Vera; Mbed TLS: Apache 2.0 or GPLv2
(or later); tcc: LGPLv2.1; bash: GPLv3; TinyX: GPLv3; UnixBench and
doomgeneric: GPLv2; Microwindows: MPL 1.1 or GPLv2.
