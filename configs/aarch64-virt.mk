# AquilaOS on QEMU's virt machine (aarch64): the kernel is built with
# clang and ld.lld, not the cross toolchain (it needs no C library); the
# system's programs with the aarch64-aquila toolchain (make toolchain)
#
#   make kernel CONFIG=aarch64-virt
#

CC := clang

# the large code model: absolute addresses (the linker script's physical
# ones, kernel_start and kernel_end, are 256 TiB from the kernel, past
# the small model's reach); no FP/SIMD in the kernel (its registers
# aren't saved on entry); frame pointers for backtraces
CFLAGS += --target=aarch64-none-elf \
		-std=gnu11 -nostdlib -ffreestanding -fno-pic -fno-pie \
		-mcmodel=large -mgeneral-regs-only -fno-omit-frame-pointer \
		-fno-stack-protector -fno-asynchronous-unwind-tables \
		-O2 -Wall -Wextra -Werror \
		-Wno-unused -Wno-unused-parameter

AS := $(CC)
ASFLAGS := $(CFLAGS)
LD := ld.lld
LDFLAGS := -nostdlib -z max-page-size=0x1000

# the boot loader (QEMU's -kernel) takes Linux's arm64 Image: the kernel's
# memory image, flat, with its header (boot/head.S)
KERNEL_BINARY := y
OBJCOPY := llvm-objcopy

# make run: QEMU's machine (no KVM on another CPU)
QEMU_MACHINE := -M virt -cpu cortex-a72
KVM := 0

#
# Configurations
#

ARCH=aarch64
ARCH_DIR=aarch64
# the system's compilers
TARGET=aarch64-aquila
ARCH_AARCH64=y
ARCH_BITS=64

#
# Platform - QEMU's virt machine: GICv2, the generic timer, a PL011
# UART, a PL031 RTC, PSCI; what's where comes from its devicetree
#

PLATFORM_AARCH64_VIRT=y
PLATFORM_DIR=virt

#
# Devices
#

DEV_UART=y
# the ARM PrimeCell UART (PL011)
DEV_UART_PL011=y
DEV_PTY=y
DEV_MEMDEV=y
# PCI Express (ECAM)
DEV_PCI=y
DEV_PCI_ECAM=y
# Intel 8254x Ethernet (e1000), QEMU's -device e1000
DEV_E1000=y

# Filesystems
FS_TMPFS=y
FS_DEVFS=y
FS_DEVPTS=y
FS_EXT2=y
FS_PROCFS=y
FS_MINIX=y

# Initramfs
INITRAMFS_CPIO=y

# Network
NET_UNIX=y
NET_INET=y
