# Devices and terminals

A UNIX kernel shows devices to programs as files. A program opens
`/dev/hda`, `/dev/tty1` or `/dev/null` and reads and writes it like any
other file; what happens behind the read is the device's business. This
one idea takes care of most of the interface: `cat /dev/hda1 > disk.img`
works without anyone having written a disk-copying program.

This document covers how a file becomes a device (device numbers, the
device layer, `/dev`), the two kinds of devices (block and character),
the disks, and the largest of the character devices: terminals, with
the line discipline, virtual terminals, serial ports and
pseudo-terminals. It ends with the smaller devices and PCI.

The device layer, the block cache, the line discipline and the
pseudo-terminals are the kernel's own, the same on any machine. Drivers
are generic code for a particular device: the ATA driver would work on
any machine with an IDE controller. Drivers are told where their device
is and which interrupt it raises: by the bus (PCI), or by the platform's
table of the devices no bus finds, and they install their handlers with
`irq_install()` (see "Devices: drivers and the platform" in
[The kernel and the machine](architecture.md)). Some of the devices
AquilaOS drives exist only on PCs (the i8042 keyboard controller, VGA
text mode): sections about those say "PC" in their title.

## The kernel's side

### Device numbers

A device is named by a **device number**, two small integers: the
*major* number says which driver, the *minor* number which of its
devices. A device node, a special file in `/dev`, is nothing but a type
(character or block) and a device number; its name means nothing to
the kernel. `mknod /dev/hda1 b 3 1` makes "block device, major 3, minor
1", which is the ATA driver's first partition of the first disk.

AquilaOS packs the two into a `dev_t` with `DEV(major, minor)`, eight bits
each ([`include/dev/dev.h`](../../kernel/include/dev/dev.h)); `DEV_MAJOR()`
and `DEV_MINOR()` take them apart. The kernel makes no nodes itself,
except `/dev/console`: `/etc/runit/1` makes them at boot with `mknod`,
in devfs (see [Filesystems](filesystems.md)).

| Major | Type  | Driver                                   | Minors                              |
|-------|-------|------------------------------------------|-------------------------------------|
| 1     | char  | memory devices (`dev/mem/`)              | 3 `null`, 5 `zero`, 8 `random`, 9 `urandom`, 11 `kmsg` |
| 1     | block | the ramdisk (`dev/rd/`)                  | 0                                   |
| 2     | char  | pseudo-terminal masters (`dev/tty/pty/`) | one per pty                         |
| 3     | block | ATA disks (`dev/ata/`)                   | 64 × disk + partition (0: the whole disk) |
| 4     | char  | terminals (`dev/tty/`)                   | 0 the foreground VT, 1–6 `tty1`–`tty6`, 64 and up `ttyS0`... |
| 5     | char  | terminals' aliases                       | 0 `tty`, 1 `console`, 2 `ptmx`       |
| 7     | char  | virtual terminals' screens               | N `vcsN`, 128 + N `vcsaN`           |
| 10    | char  | the PS/2 mouse (`dev/mouse/`)            | 1 `mouse`                           |
| 29    | char  | framebuffers (`dev/fb/`)                 | 0 `fb0`                             |
| 136   | char  | pseudo-terminal slaves                   | N `/dev/pts/N`                      |

The numbers are Linux's, so programs ported from Linux find what they
expect.

### The device layer

The device layer, `kdev` ([`dev/kdev.c`](../../kernel/dev/kdev.c)), is a
table: for each major number, the `struct dev` of its driver, one table
for character devices and one for block devices. A driver registers
itself when it starts (`kdev_chrdev_register()`,
`kdev_blkdev_register()`), from its `MODULE_INIT()` function (see
[Booting](booting.md)).

```c
struct dev {
    char    *name;
    ssize_t (*read) (struct devid *dev, off_t offset, struct iobuf *io);
    ssize_t (*write)(struct devid *dev, off_t offset, struct iobuf *io);
    ssize_t (*bread) (struct devid *dev, off_t lba, size_t count, void *buf);
    ssize_t (*bwrite)(struct devid *dev, off_t lba, size_t count, void *buf);
    int     (*ioctl)(struct devid *dev, int request, void *argp);
    int     (*map)  (struct devid *dev, struct vm_space *vm_space, struct vm_entry *vm_entry);
    struct fops fops;                         /* open, read, close, poll... */
    struct dev *(*mux)(struct devid *dev);    /* the driver of this minor */
    size_t  (*getbs)(struct devid *dev);      /* block size */
    int     (*sync)(void);                    /* write caches to the media */
    ...
};
```

When the VFS opens or reads a device node, it calls `kdev_file_open()`,
`kdev_read()` and so on with the node's `struct devid` (type, major,
minor); `kdev` finds the driver and calls its function. A missing
function is `ENXIO`, except `lseek`, which is `ESPIPE` (terminals and
other streams can't seek), and `close`, which only drops the vnode's
reference (`posix_file_close()`).

One major number can hold several drivers: a `mux` function picks the
driver for a minor. Major 1 is `/dev/null`, `/dev/zero`, `/dev/random`
and `/dev/kmsg`, each its own driver; major 4 is virtual terminals below
64 and serial ports from 64 on.

### Character and block devices

The two kinds differ in what they move:

- A **character device** is a stream of bytes (a terminal, a mouse,
  `/dev/null`): its `read` and `write` take any number of bytes, through
  an `iobuf` that may be a process's memory.
- A **block device** is an array of fixed-size blocks (a disk, 512 bytes
  each): its `bread` and `bwrite` take whole blocks into kernel memory.
  `kdev` makes bytes of them (`kdev_bread()`, `kdev_bwrite()`): a read
  that starts or ends inside a block reads the whole block into a buffer
  and copies the part asked for; a write in part of a block reads it,
  changes it and writes it back. Filesystems use these, and so does
  `cat /dev/hda1`.

### Disks

A disk is a block device. Two things sit between a filesystem and the
driver: partitions and the block cache.

**Partitions.** A PC disk starts with a *master boot record* (MBR): boot
code, then a table of four partitions, each a first sector and a number
of sectors ([`include/fs/mbr.h`](../../kernel/include/fs/mbr.h)). The
minor number names the partition (`/dev/hda2` is minor 2); the driver
reads the table the first time a partition is used, and adds the
partition's first sector to every block number. A partition that isn't
there is `ENXIO`; a write past its end, `ENOSPC`. Only the MBR's four
primary partitions are read: no extended partitions, no GPT.

**The block cache** ([`dev/blkcache.c`](../../kernel/dev/blkcache.c)).
Disks are slow, and filesystems read the same blocks again and again (a
directory, an inode table, a bitmap). The block cache keeps what disks
read and write, 8 MiB of it, in 4 KiB blocks of 8 sectors:

- It's **write-back**: a write changes the cached block and returns. The
  dirty blocks go to the disk at `sync()`, at restart and power-off,
  when half the cache is dirty, or when the oldest has been dirty 5
  seconds (checked at the next use of the cache). Runit's `update`
  service calls `sync` every 30 seconds too, as the BSDs' `update` did.
- Each sector of a block is *valid* or not and *dirty* or not, so a
  write of one sector doesn't read the other seven first.
- Blocks are found by a hash of their number; when the cache is full, the
  clean block used least recently makes room.
- Blocks are kept by their sector on the disk, after the partition's
  offset, so a sector is cached once whichever node it's read through.
- The driver only ever transfers to and from the cache's own buffers,
  never a process's: a process's buffer may not be mapped yet, and
  mapping it might read the disk, the driver waiting for itself.

`kdev_sync()` writes the cache back, then asks each block driver to
flush its disk's own write cache (`sync`).

**The ramdisk** ([`dev/rd/ramdisk.c`](../../kernel/dev/rd/ramdisk.c)) is
the first boot module, the initramfs, shown as block device 1: reads and
writes copy from and to the memory where the boot loader left it. Its
block size is 1 byte.

### Terminals

A terminal is where a user types and reads: once a real terminal on a
serial line, now a window, the screen of the console, or an SSH
connection. Programs see the same interface for all of them, defined by
POSIX: `termios`.

Between the device and the program sits the **line discipline**, the
kernel code that makes a stream of key presses into what a program
reads, and a program's output into what the device shows
([`dev/tty/generic.c`](../../kernel/dev/tty/generic.c)). Every terminal
has a `struct tty` ([`include/dev/tty.h`](../../kernel/include/dev/tty.h)):
its `termios` settings, its window size, the line being edited, and its
session and foreground process group. The device gives the tty two
functions: `master_write` sends output to the device (the screen, the
serial line, the pty master), and `slave_write` queues input for
readers.

**Input** (`tty_master_write()`), each character the device received:

1. **Translation** (`c_iflag`): a CR becomes NL (`ICRNL`), or is dropped
   (`IGNCR`); NL may become CR (`INLCR`).
2. **Signals** (`ISIG`): `^C` (`VINTR`) sends `SIGINT` to the foreground
   process group, `^\` (`VQUIT`) `SIGQUIT`, `^Z` (`VSUSP`) `SIGTSTP`. The
   signal is only queued: this may run in an interrupt.
3. **Canonical mode** (`ICANON`), the default: the line is collected and
   edited (`VERASE` erases a character, `VKILL` the line) and given to
   readers only when it ends, with NL, `VEOL` or `^D` (`VEOF`). `^D` at
   the start of a line is an end of file: the next read returns 0.
   Without `ICANON`, every character goes to readers as it comes: a
   read waits for at least one, and returns what there is. With `VMIN`
   0, `VTIME` says how long it waits, in tenths of a second, and with
   both 0 it doesn't wait. A `VMIN` above 1 counts as 1, and there is no
   timer between characters.
4. **Echo** (`ECHO`): what's typed is shown, control characters as `^X`;
   an erase is shown as backspace, space, backspace (`ECHOE`).

**Output** (`tty_slave_write()`), with `OPOST`: NL is sent as CR NL
(`ONLCR`, the default), CR as NL (`OCRNL`), a CR in the first column is
dropped (`ONOCR`), and a NL may return the carriage too (`ONLRET`). For
these the tty follows the cursor's column (`col`), counting tabs and
backspaces. If the device takes the CR of a CR NL but not the NL, the
NL is owed and goes first next time (`nl_owed`).

`ioctl()`s read and change all of this: `TCGETS`, `TCSETS` (and
`TCSETSW`, `TCSETSF`), `TCFLSH`, `TIOCGWINSZ`, `TIOCSWINSZ`, and the job
control ones below (`TIOCSCTTY`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCGSID`).
What libc's `tcgetattr()`, `tcsetattr()` and `cfmakeraw()` call is
these.

**Flow control** (`IXON`, the default): `^S` (`VSTOP`) stops the
output, and `^Q` (`VSTART`), or any character with `IXANY`, starts it
again; neither is input. While it is stopped, programs that write to the
terminal wait.

**Readiness**: for `select()` and `poll()`, a terminal is readable only
when it holds input or a pending end of file (`tty_can_read()`), even
with `VMIN` and `VTIME` 0, as on Linux and the BSDs.

Shortcuts: no output is ever waiting to be sent, so `TCSETSW` is
`TCSETS`, `tcdrain()` returns at once, and `TCSBRK` and `TCXONC` do
nothing; the speed in `termios` isn't used by any driver.

### Controlling terminals and job control

A **session** is a login: a shell and everything started from it. Its
processes are in **process groups**, one per job (`ls | less` is one
group), and the terminal has one **foreground** group, the one that gets
the keyboard's signals and may read (see
[Processes](processes.md) and [Signals](signals.md)).

- The session leader's first terminal it opens becomes its
  **controlling terminal**, unless it opens it with `O_NOCTTY`, the
  session already has one, or the terminal already belongs to a session
  (`tty_open()`); `/dev/console` never becomes one;
  `TIOCSCTTY` asks for it explicitly. `/dev/tty` (5, 0) opens the
  caller's controlling terminal, whichever it is.
- The shell moves jobs to the foreground with `TIOCSPGRP` (`tcsetpgrp()`),
  and reads the foreground group with `TIOCGPGRP`.
- A process of a background group that reads its terminal gets
  `SIGTTIN`; one that writes, with `TOSTOP` set, or changes the
  terminal's settings, gets `SIGTTOU` (`tty_background()`). Its group
  stops, and the call is made again once the shell continues it in the
  foreground. With the signal ignored or blocked, a read fails with
  `EIO` and a write goes on; a group with no parent in the session
  (*orphaned*) isn't stopped, since nobody would continue it.
- When the session leader exits or the terminal goes away, the
  foreground group gets `SIGHUP` and `SIGCONT`, and the terminal belongs
  to no session (`tty_hangup()`).

### Pseudo-terminals

A **pseudo-terminal** (pty) is a terminal with a program instead of a
device at the other end: a terminal emulator, `script`, an SSH server.
It is a pair: the program holds the **master**, and the programs it
runs use the **slave** as their terminal. What the master writes is
input to the slave, through the line discipline, as if typed; what the
slave's programs write comes out of the master
([`dev/tty/pty/pty.c`](../../kernel/dev/tty/pty/pty.c)).

Opening `/dev/ptmx` (5, 2) makes a new pty: the descriptor becomes its
master (major 2, which has no node), and the slave appears in devpts as
`/dev/pts/N` (major 136, minor N); `TIOCGPTN` says which N. There are at
most 256 ptys. Each side has a 4 KiB ring. When the master is closed,
the slave's session is hung up, the slave's node goes, and slaves still
open fail with `ENXIO`. `grantpt()` and `unlockpt()` have nothing to do:
the slave is the opener's from the start, and never locked.

### The console

`/dev/console` (5, 1) is where init and the kernel write: on AquilaOS,
the first virtual terminal (below), or, on a machine without virtual
terminals (aarch64's virt), the first UART. The kernel's own messages
(`printk()`) go to the foreground virtual terminal once there is one, and
to a ring that `/dev/kmsg` reads. Before the terminals are
up, and all along without them, they go through the early console (see
[Booting](booting.md)).

### Memory devices

- `/dev/null` reads nothing and takes everything; `/dev/zero` reads
  zeros.
- `/dev/random` and `/dev/urandom`, the same device
  ([`dev/mem/random.c`](../../kernel/dev/mem/random.c)): ChaCha20 used
  as a generator, keyed from a pool that the CPU's cycle counter
  (`arch_cycles()`) is stirred into at every tick and every request.
  There is no hardware source: it is good enough for hash seeds and
  `arc4random()`, not for generating keys.
- `/dev/kmsg`: the kernel's messages so far.

## PC: the ATA disks

The **ATA** (IDE) driver ([`dev/ata/`](../../kernel/dev/ata/ata.c)) drives
the parallel ATA disks of an IDE controller, the kind QEMU's PC has
(PIIX). The controller is found on PCI (class 1, subclass 1); it has two
**channels**, each with a **master** and a **slave** drive, so four
drives: `hda`, `hdb` on the first channel, `hdc`, `hdd` on the second
(minors 0, 64, 128, 192).

- Each drive is reset and sent `IDENTIFY`, which gives its size and
  whether it does 48-bit LBA. SATA and ATAPI (CD) drives are found but
  not supported.
- A channel's registers are I/O ports. In the controller's
  *compatibility mode* they're the ISA ones (`0x1F0` and `0x170`), with
  IRQs 14 and 15: these are PC-specific; in *native mode* the PCI BARs
  say, with the PCI interrupt line.
- One command at a time runs on a channel; a request waits for it
  (`channel_acquire()`).
- **PIO** ([`pio.c`](../../kernel/dev/ata/pio.c)) moves data a 16-bit word
  at a time through the data port, polling the drive. **Bus master DMA**
  ([`dma.c`](../../kernel/dev/ata/dma.c)), when the controller has it
  (BAR 4), lets the controller move a whole command's data itself,
  through a 64 KiB buffer described by a *physical region descriptor*,
  and the request sleeps until the drive's interrupt. Each channel's
  buffer is a block of the buddy allocator, below 16 MiB: aligned to its
  size, it crosses no 64 KiB boundary, as the controller requires. DMA is used when
  available; `ata.nodma` on the command line keeps to PIO. Under a
  hypervisor PIO is especially slow: every port access exits to it.

## Virtual terminals

**Virtual terminals** (VTs) make the screen and keyboard into six
terminals, `/dev/tty1` to `/dev/tty6`, switched with Alt+F1 to Alt+F6
([`dev/tty/vt/vt.c`](../../kernel/dev/tty/vt/vt.c)). Only the one in the
foreground is drawn; the others keep their screens. `/dev/tty0` is
whichever is in the foreground.

- **The terminal emulator** ([`term.c`](../../kernel/dev/tty/vt/term.c))
  does what the Linux console does with escape sequences (programs are
  told `TERM=linux`), with the parts of xterm programs commonly use: the
  alternate screen, 256 colors. Each terminal is a grid of cells
  (a character, its attributes and colors); only cells that changed are
  redrawn. Characters are code page 437's 256 glyphs; UTF-8 is decoded
  to them.
- **The displays**: the framebuffer, if the boot loader set a graphics
  mode, drawn with an 8×16 font ([`fb.c`](../../kernel/dev/tty/vt/fb.c));
  otherwise VGA text mode, 80×25, where the hardware draws the
  characters from the memory at `0xB8000`
  ([`vga.c`](../../kernel/dev/tty/vt/vga.c)). VGA text mode is PC
  hardware.
- **The keyboard** ([`keyboard.c`](../../kernel/dev/tty/vt/keyboard.c)):
  the PS/2 keyboard's scancodes (set 1) become what the Linux console
  sends for each key, with the US layout. Ctrl+Alt+Del restarts (or
  tells init, see `sys/reboot.c`).
- For programs that draw the screen themselves, such as an X server,
  Linux's console `ioctl()`s: `KDSETMODE` (the VT isn't drawn),
  `KDSKBMODE` (raw scancodes or key codes instead of characters), and
  `VT_*` to switch terminals or be asked first. When the process that
  set them is gone, they're undone.
- `/dev/vcsN` and `/dev/vcsaN` read a VT's screen, as on Linux.

The VT is a device of the tty layer like any other: it gives the tty its
`master_write` (into the emulator) and takes the line discipline's
output for readers. The VT layer and the emulator are the kernel's; VGA
text mode and the PS/2 scancodes are the PC's.

## PC: the keyboard and mouse

The PC's keyboard and mouse hang off the **i8042** PS/2 controller
([`dev/input/i8042.c`](../../kernel/dev/input/i8042.c), `DEV_I8042`),
a device in the PC's table: ports `0x60` and `0x64`, the keyboard's
port on IRQ 1, the mouse's on IRQ 12. The driver turns the second port
on; the keyboard and mouse drivers register a function for a port
(`i8042_handler_register()`), which gets each byte that comes, and the
mouse's sends it bytes (`i8042_aux_write()`):

- the keyboard's ([`dev/kbd/ps2kbd.c`](../../kernel/dev/kbd/ps2kbd.c))
  goes to the virtual terminals;
- the mouse's ([`dev/mouse/ps2mouse.c`](../../kernel/dev/mouse/ps2mouse.c))
  is `/dev/mouse` (10, 1): the mouse's packets as they come, as Linux's
  `/dev/psaux`; what is written goes to the mouse.

## Serial ports

A **UART** is a serial port. The tty side, generic, is in
[`dev/tty/uart/uart.c`](../../kernel/dev/tty/uart/uart.c): a UART is a
terminal, `/dev/ttyS0` and up (4, 64 and up), with a 4 KiB ring each way.
Two drivers:

- the **8250** ([`8250/8250.c`](../../kernel/dev/tty/uart/8250/8250.c)),
  the 16550-compatible UART: one for each `"8250"` in the platform's
  table that answers (its scratch register keeps what's written), in
  the table's order. On the PC, COM1 (port `0x3F8`, IRQ 4) is
  `/dev/ttyS0` and COM2 (`0x2F8`, IRQ 3), if QEMU has one, `/dev/ttyS1`.
  38400 baud, 8 bits, no parity; its FIFO is drained at every interrupt;
  its registers may be ports or memory, spaced 1, 2 or 4 bytes apart
  (`IOADDR_MMIO8`, `MMIO16`, `MMIO32`).
- the **PL011** ([`pl011/pl011.c`](../../kernel/dev/tty/uart/pl011/pl011.c)),
  ARM's UART, the virt machine's `/dev/ttyS0` (and `/dev/console`, which
  has no virtual terminals there).

Output is written a byte at a time, waiting for the transmitter.

## The framebuffer

`/dev/fb0` (29, 0, [`dev/fb/fbdev/fbdev.c`](../../kernel/dev/fb/fbdev/fbdev.c))
is the screen as memory, with Linux's framebuffer `ioctl()`s
(`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, ...): a program can `mmap()`
it and draw. The only framebuffer driver, `vesa`
([`dev/fb/vesa/vesa.c`](../../kernel/dev/fb/vesa/vesa.c)), is the linear
framebuffer the boot loader set up: the PC's Multiboot code registers it
(see [Booting](booting.md)). AquilaOS can't change the mode itself.

## PCI

**PCI** is how devices on a modern PC (and many other machines) are
found: each one has a *configuration space*, 256 bytes saying who made
it (vendor and device IDs), what it is (class and subclass), which
memory and ports it decodes (its *base address registers*, BARs), its
interrupt line, and a list of *capabilities*
([`dev/pci/pci.c`](../../kernel/dev/pci/pci.c)).

- **Finding devices**: a driver asks for its device by vendor and
  device ID (`pci_device_scan()`) or by class (`pci_scan_device()`). Only
  bus 0 is scanned, which is enough for QEMU's default PC (i440FX) and
  its virt machine, where every device is on it; devices behind a bridge
  aren't found.
- **BARs** (`pci_bar()`): the address, and the size, found by writing
  all ones and reading back what the device kept; 64-bit memory BARs use
  two. `arch_mmio_map()` maps a memory BAR's registers uncached.
- **Enabling**: `pci_enable()` turns on its memory and port decoding and
  bus mastering (DMA).
- **Interrupts**: the IRQ its pin comes to (`pci_irq()`), or MSI
  (`pci_msi_enable()`, see [Interrupts](interrupts.md)).

**The host bridge** is the platform's, a `struct pci_host` it gives
`pci_host_set()` in `platform_init()`:

- how configuration space is reached: its `read` and `write`, with
  either of the kernel's two mechanisms,
  [`conf1.c`](../../kernel/dev/pci/conf1.c) (`DEV_PCI_CONF1`) or
  [`ecam.c`](../../kernel/dev/pci/ecam.c) (`DEV_PCI_ECAM`);
- which IRQs INTA# to INTD# come to, if the platform knows: a device's
  pin is swizzled by its slot (INTA# of slot 1 is INTB#), as a host
  bridge's interrupt map does. If it doesn't, the interrupt line
  register's, which the firmware wrote;
- where the BARs go if no firmware assigned them: windows of memory and
  ports, which `pci_host_set()` fills, each BAR aligned to its size.

**PC: configuration mechanism #1.** Ports `0xCF8` (an address: bus,
device, function, register) and `0xCFC` (the data), the first 256 bytes
of a function's configuration space. The BIOS assigned the BARs and
wrote the interrupt lines.

**virt: ECAM.** PCI Express's Enhanced Configuration Access Mechanism:
every function's 4 KiB in memory, bus 0's 1 MiB of it mapped
(`0x40_1000_0000` on QEMU's virt machine). Nothing ran before the kernel
(`-kernel`): it assigns the BARs itself, memory from `0x1000_0000`,
ports from `0x1000`, and INTA# to INTD# are the GIC's SPIs 3 to 6. QEMU's
`-device e1000` works there unchanged.

## Where it is

| File                                       | Layer  | What                                         |
|--------------------------------------------|--------|----------------------------------------------|
| `kernel/include/dev/dev.h`                 | kernel | `struct dev`, device numbers                 |
| `kernel/dev/kdev.c`                        | kernel | the device layer, blocks to bytes            |
| `kernel/dev/blkcache.c`                    | kernel | the block cache                              |
| `kernel/dev/rd/ramdisk.c`                  | kernel | the ramdisk                                  |
| `kernel/dev/tty/generic.c`                 | kernel | the line discipline, job control             |
| `kernel/dev/tty/tty.c`                     | kernel | `/dev/tty`, `/dev/console`, major 4's minors |
| `kernel/dev/tty/pty/pty.c`, `fs/devpts/`   | kernel | pseudo-terminals                             |
| `kernel/dev/tty/uart/uart.c`               | kernel | serial ports as terminals                    |
| `kernel/dev/tty/vt/vt.c`, `term.c`         | kernel | virtual terminals, the terminal emulator     |
| `kernel/dev/tty/vt/fb.c`                   | kernel | VTs on a framebuffer                         |
| `kernel/dev/mem/`                          | kernel | `null`, `zero`, `random`, `kmsg`             |
| `kernel/dev/fb/fbdev/fbdev.c`              | kernel | `/dev/fb0`                                   |
| `kernel/dev/platform.c`                    | kernel | the platform's devices: `platform_device_get()` |
| `kernel/dev/pci/pci.c`                     | kernel | PCI: devices, BARs, MSI, the host bridge     |
| `kernel/dev/pci/conf1.c`, `ecam.c`         | kernel | PCI configuration space: ports, memory       |
| `kernel/dev/ata/`                          | kernel, PC | ATA disks (ISA ports and IRQs in compatibility mode) |
| `kernel/dev/net/e1000.c`                   | kernel | the e1000 network card (see [Networking](networking.md)) |
| `kernel/dev/tty/vt/vga.c`                  | PC     | VTs in VGA text mode                         |
| `kernel/dev/tty/vt/keyboard.c`             | PC     | PS/2 scancodes to the console's keys         |
| `kernel/dev/kbd/ps2kbd.c`, `dev/mouse/ps2mouse.c` | PC | the PS/2 keyboard and mouse             |
| `kernel/dev/tty/uart/8250/8250.c`          | kernel | 8250/16550 serial ports                      |
| `kernel/dev/tty/uart/pl011/pl011.c`        | kernel | the PL011 serial port                        |
| `kernel/dev/fb/vesa/vesa.c`                | PC     | the boot loader's framebuffer                |
| `kernel/dev/input/i8042.c`                 | PC     | the PS/2 controller                          |
| `kernel/arch/i386/platform/pc/devices.c`   | PC     | the PC's devices: serial ports, the i8042    |
| `kernel/arch/i386/platform/pc/init.c`      | PC     | PCI's host bridge: mechanism #1              |
| `kernel/arch/aarch64/platform/virt/devices.c` | virt | the virt machine's devices: the PL011      |
| `kernel/arch/aarch64/platform/virt/init.c` | virt   | PCI's host bridge: ECAM, BARs, INTx          |
| `rootfs/etc/runit/1`                       | —      | the device nodes, made at boot               |

## References

- *POSIX.1-2017*, *General Terminal Interface* (chapter 11 of the Base
  Definitions) and `termios.h`: canonical mode, the special characters,
  the flags, job control.
- W. Richard Stevens and Stephen Rago, *Advanced Programming in the UNIX
  Environment*, chapters 9 (sessions, process groups, controlling
  terminals), 18 (terminal I/O) and 19 (pseudo terminals).
- *AT Attachment with Packet Interface (ATA/ATAPI-6)*, T13: the
  registers, `IDENTIFY DEVICE`, 48-bit LBA. *Programming Interface for
  Bus Master IDE Controller*, revision 1.0: bus master DMA and the PRD
  table.
- *PC16550D Universal Asynchronous Receiver/Transmitter with FIFOs*,
  National Semiconductor: the serial port's registers.
- *PCI Local Bus Specification*, revision 3.0: configuration space,
  BARs, capabilities, configuration mechanism #1.
- Linux's `console_codes(4)`, `ioctl_console(2)` and `ioctl_tty(2)`
  manual pages: the escape sequences and `ioctl()`s the virtual
  terminals implement.
- The OSDev wiki's pages on [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode),
  [PCI IDE Controller](https://wiki.osdev.org/PCI_IDE_Controller),
  [Serial Ports](https://wiki.osdev.org/Serial_Ports),
  [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) and
  [PCI](https://wiki.osdev.org/PCI).
