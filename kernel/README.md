# The AquilaOS kernel

How it works, subsystem by subsystem, is in
[docs/kernel/](../docs/kernel/README.md); start there.

| directory  |                                                              |
|------------|--------------------------------------------------------------|
| `core/`    | `kmain()`, printk, kernel arguments, module init, the wall clock (`panic()` is in `include/core/panic.h`) |
| `mm/`      | physical pages (buddy), the kernel heap (slabs and kvmem), address spaces, vm objects (the page cache), page faults |
| `sys/`     | processes, threads, the scheduler, system calls, signals     |
| `fs/`      | the VFS, and initramfs, tmpfs, devfs, devpts, procfs, ext2, minix |
| `dev/`     | the device layer and the drivers                             |
| `net/`     | Unix sockets, IPv4 (ARP, ICMP, UDP, TCP)                     |
| `include/` | the kernel's headers                                         |
| `arch/`    | `i386/` (also x86_64, `x86_64` is a link to it) and `aarch64/`, each with its platforms |
