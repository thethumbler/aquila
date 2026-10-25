#
# AquilaOS top level Makefile
#
# Everything is built into $(BUILDDIR): build/<arch>/ (build/x86_64,
# build/aarch64), build/ itself for i386. The paths below that don't start
# with build/ are in it.
#
#   make toolchain    the cross toolchain and C library (x86_64-aquila,
#                     i686-aquila or aarch64-aquila: build/toolchain, and
#                     build/sysroot-<arch> or build/sysroot for i386),
#                     needed by the rest
#   make              kernel, user space programs, base third-party
#                     software (contrib/) and initrd image
#   make kernel       kernel/kernel-$(VERSION).$(ARCH)
#   make userland     userland/bin/
#   make contrib      third-party software of the base system in contrib/
#                     (contrib/root): dash, sbase, runit, netcat, sdhcp
#   make ports        optional third-party software in ports/
#                     (ports/root), the out-of-date ones, included in the
#                     initrd once built
#
#   CONTRIB_ROOT=<dir> and PORTS_ROOT=<dir> take those from elsewhere.
#   make initrd       rootfs/ and initrd.img, ROOTFS_OVERLAY=<dir>
#                     is copied over the tree (e.g. etc/passwd and etc/shadow)
#   make iso          aquila.iso, boots with Limine (x86 only; needs xorriso)
#   make live         live.img (i386 only): a tiny disk (LIVE_SIZE=<KiB>,
#                     default 2048) with the base system (no ports), for v86
#                     in a browser; its root filesystem is the initrd, in
#                     memory (needs mtools and sfdisk)
#   make hdd          aquila.img (x86 only), a hard disk with AquilaOS installed:
#                     Limine on a FAT32 /boot, the root filesystem on ext2
#                     (HDD_SIZE=<MiB>, default 512; needs sfdisk, mkfs.fat,
#                     mtools, mke2fs and fakeroot). Replaces an existing one.
#   make run-min      the quick start, no toolchain: the kernel built with
#                     the host's gcc and start/init.c, a tiny shell, as the
#                     only program (needs gcc, ld, cpio and QEMU)
#   make min          build it only (build/min-$(ARCH)/; aarch64's kernel
#                     is built with clang)
#   make debug        the same with debugging information (DEBUG=1), QEMU
#                     waiting for gdb: gdb -x mk/gdbinit <the kernel it says>
#                     (mk/backtrace.sh names a panic's addresses)
#   make run          boot the kernel and initrd in QEMU, console on stdio
#   make run-iso      boot the ISO in QEMU (framebuffer console)
#   make hdd-kernel   put the kernel (and Limine's configuration) on
#                     aquila.img, its root filesystem is kept as it is
#   make hdd-update   the kernel and the system just built (rootfs/)
#                     on aquila.img: what the build has replaces the
#                     disk's, the rest is kept (/root, /home, and the
#                     accounts in /etc if the disk has them)
#   make run-hdd      boot the hard disk in QEMU with the system just built
#                     (make hdd-update), made by make hdd if there is none
#
#   QEMU uses KVM when /dev/kvm can be used, KVM=0 turns it off (also for
#   make test).
#   make test         kernel test suite (tests/run.sh, uses the host gcc)
#   make docs         the kernel's documents (docs/kernel/*.md) and its code
#                     reference, build/docs/kernel/api/ (needs doxygen)
#   make docs-serve   the same, served on http://localhost:8000/
#                     (DOCS_PORT=<port>; needs python3)
#   make clean        remove build output except the toolchain, downloads
#                     and the hard disk (it has what was saved on it)
#   make distclean    remove $(BUILDDIR) (for i386, all of build/)
#
# CONFIG selects the kernel configuration in configs/ (default: x86_64-pc),
# and the architecture of everything: x86-64 is built into build/x86_64,
# CONFIG=aarch64-virt builds aarch64 into build/aarch64, CONFIG=i386-pc
# builds i386 into build/ (the toolchains are all in build/toolchain).
#

SRCDIR   := $(CURDIR)
CONFIG   ?= x86_64-pc

include $(SRCDIR)/configs/$(CONFIG).mk

BUILDDIR ?= $(SRCDIR)/build$(if $(filter-out i386,$(ARCH)),/$(ARCH))

VERSION := 0.0.1

TOOLCHAIN ?= $(SRCDIR)/build/toolchain
SYSROOT   := $(abspath $(TOOLCHAIN)/..)/sysroot$(if $(filter-out i386,$(ARCH)),-$(ARCH))
ROOTFS    := $(BUILDDIR)/rootfs
INITRD    := $(BUILDDIR)/initrd.img
ISO       := $(BUILDDIR)/aquila.iso
HDD       := $(BUILDDIR)/aquila.img
LIVE      := $(BUILDDIR)/live.img
LIVE_SIZE ?= 2048
HDD_SIZE  ?= 512
KERNEL    := $(BUILDDIR)/kernel/kernel-$(VERSION).$(ARCH)
CONTRIB_ROOT ?= $(BUILDDIR)/contrib/root
PORTS_ROOT   ?= $(BUILDDIR)/ports/root

PATH := $(TOOLCHAIN)/bin:$(PATH)

CP    := cp
ECHO  := echo
MKDIR := mkdir -p
LN    := ln
STRIP := $(TARGET)-strip

LIMINE := $(TOOLCHAIN)/share/limine

# KVM when /dev/kvm can be used, KVM=0 emulates the CPU instead
KVM  ?= $(if $(shell test -r /dev/kvm -a -w /dev/kvm && echo y),1,0)
QEMU := qemu-system-$(ARCH) $(if $(filter 1,$(KVM)),-accel kvm) $(QEMU_MACHINE)

# what QEMU's -kernel boots: the kernel's flat image if it has one
# (KERNEL_BINARY: aarch64's), its ELF file else
RUN_KERNEL := $(if $(filter y,$(KERNEL_BINARY)),$(KERNEL).img,$(KERNEL))

MAKEFLAGS += --no-print-directory

export

.PHONY: all toolchain kernel userland contrib ports initrd iso live hdd hdd-kernel hdd-update run run-iso run-hdd min run-min debug test docs docs-serve clean distclean check-toolchain

all: kernel userland contrib initrd

toolchain:
	@BUILDDIR=$(abspath $(TOOLCHAIN)/..) $(SRCDIR)/toolchain/build.sh

check-toolchain:
	@command -v $(TARGET)-gcc >/dev/null || { \
		echo "error: $(TARGET)-gcc not found, run 'make toolchain' first" >&2; exit 1; }

# the kernel can also be built with the host compiler (see tests/host/)
kernel:
	@command -v $(CC) >/dev/null || { \
		echo "error: $(CC) not found, run 'make toolchain' first" >&2; exit 1; }
	@$(MAKE) -f $(SRCDIR)/kernel/Makefile

userland: check-toolchain
	@$(MAKE) -f $(SRCDIR)/userland/Makefile

#
# Third-party software: contrib/ (base system) and ports/ (optional)
#

RECIPES := $(SRCDIR)/mk/recipes.sh
CONTRIB := $(patsubst $(SRCDIR)/contrib/%/recipe.sh,%,$(wildcard $(SRCDIR)/contrib/*/recipe.sh))

# rebuilt when its recipe or patches, the build script or libc change
contrib: $(CONTRIB:%=$(BUILDDIR)/contrib/%.built)

.SECONDEXPANSION:
$(BUILDDIR)/contrib/%.built: $$(wildcard $(SRCDIR)/contrib/$$*/*) $(RECIPES) \
		$$(wildcard $(SYSROOT)/usr/lib/libc.a) | check-toolchain
	@BUILDDIR=$(BUILDDIR) TOOLCHAIN=$(TOOLCHAIN) $(RECIPES) contrib $*
	@touch $@

ports: check-toolchain
	@BUILDDIR=$(BUILDDIR) TOOLCHAIN=$(TOOLCHAIN) $(RECIPES) ports

#
# Root filesystem and initrd
#

# what the base system has besides contrib/ (sbase, dash)
AQBOX_BIN  := clear ping ps stat truncate vmstat
AQBOX_SBIN := getty ifconfig login mount poweroff readmbr reboot route

# contrib/ is built unless it comes from elsewhere
initrd: userland $(if $(filter $(BUILDDIR)/contrib/root,$(CONTRIB_ROOT)),contrib)
	@$(ECHO) "  ROOTFS  " $(patsubst $(BUILDDIR)/%,build/%,$(ROOTFS))
	@$(RM) -r $(ROOTFS)
	@$(MKDIR) $(ROOTFS)
	@cd $(ROOTFS) && $(MKDIR) bin sbin dev mnt proc run tmp root home/live
	@$(CP) -r $(SRCDIR)/rootfs/. $(ROOTFS)/
	@# no virtual terminals (aarch64's virt): no logins on them
	@$(if $(filter y,$(DEV_CONSOLE)),true,$(RM) $(ROOTFS)/etc/service/getty-tty[0-9])
	@if [ -n "$(ROOTFS_OVERLAY)" ]; then $(CP) -r $(ROOTFS_OVERLAY)/. $(ROOTFS)/; fi
	@# runit is init (contrib/runit)
	@$(LN) -s /sbin/runit-init $(ROOTFS)/init
	@$(CP) $(BUILDDIR)/userland/bin/aqbox $(ROOTFS)/bin/
	@chmod 600 $(ROOTFS)/etc/shadow
	@for a in $(AQBOX_BIN); do $(LN) -s /bin/aqbox $(ROOTFS)/bin/$$a; done
	@for a in $(AQBOX_SBIN); do $(LN) -s /bin/aqbox $(ROOTFS)/sbin/$$a; done
	@test -d $(CONTRIB_ROOT) || { echo "error: $(CONTRIB_ROOT) not found" >&2; exit 1; }
	@# replace, not write through, a symbolic link of the same name
	@$(CP) -r --remove-destination $(CONTRIB_ROOT)/. $(ROOTFS)/
	@if [ -d $(PORTS_ROOT) ]; then \
		$(CP) -r --remove-destination $(PORTS_ROOT)/. $(ROOTFS)/; fi
	@# programs, not object files (crt*.o), whose symbols linking needs
	@find $(ROOTFS) -type f ! -name '*.o' -exec sh -c \
		'for f; do if head -c 4 "$$f" | grep -q ELF; then $(STRIP) "$$f"; fi; done' _ {} +
	@$(ECHO) "  CPIO    " $(patsubst $(BUILDDIR)/%,build/%,$(INITRD))
	@# the kernel's initramfs reads old binary cpio archives
	@cd $(ROOTFS) && find . | cpio -o -H bin -R 0:0 --quiet > $(INITRD)

#
# Bootable image
#

iso: kernel initrd
	@test -f $(LIMINE)/limine-bios-cd.bin || { \
		echo "error: Limine not found, run 'make toolchain'" >&2; exit 1; }
	@$(ECHO) "  ISO     " $(patsubst $(BUILDDIR)/%,build/%,$(ISO))
	@$(RM) -r $(BUILDDIR)/iso
	@$(MKDIR) $(BUILDDIR)/iso/boot/limine
	@$(CP) $(KERNEL) $(BUILDDIR)/iso/boot/kernel
	@$(CP) $(INITRD) $(BUILDDIR)/iso/boot/initrd.img
	@$(CP) $(SRCDIR)/boot/limine.conf $(LIMINE)/limine-bios.sys \
		$(LIMINE)/limine-bios-cd.bin $(BUILDDIR)/iso/boot/limine/
	@xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		$(BUILDDIR)/iso -o $(ISO) 2>/dev/null

# the base system's initrd, without the ports (build/live/), on a tiny disk
live: kernel userland
	@test "$(ARCH)" = i386 || { echo "error: the live disk is i386's, make live CONFIG=i386-pc" >&2; exit 1; }
	@test -x $(TOOLCHAIN)/bin/limine || { \
		echo "error: Limine not found, run 'make toolchain'" >&2; exit 1; }
	@$(MAKE) initrd ROOTFS=$(BUILDDIR)/live/rootfs INITRD=$(BUILDDIR)/live/initrd.img \
		PORTS_ROOT=/nonexistent
	@$(ECHO) "  LIVE    " $(patsubst $(BUILDDIR)/%,build/%,$(LIVE))
	@$(SRCDIR)/mk/live.sh $(LIVE) $(LIVE_SIZE) $(KERNEL) $(BUILDDIR)/live/initrd.img \
		$(SRCDIR)/boot/limine-live.conf $(LIMINE) $(TOOLCHAIN)/bin/limine $(STRIP)

hdd: kernel initrd
	@test -x $(TOOLCHAIN)/bin/limine || { \
		echo "error: Limine not found, run 'make toolchain'" >&2; exit 1; }
	@$(ECHO) "  HDD     " $(patsubst $(BUILDDIR)/%,build/%,$(HDD))
	@$(SRCDIR)/mk/hdd.sh $(HDD) $(HDD_SIZE) $(ROOTFS) $(KERNEL) \
		$(SRCDIR)/boot/limine-hdd.conf $(LIMINE) $(TOOLCHAIN)/bin/limine

hdd-kernel: kernel
	@test -f $(HDD) || { \
		echo "error: $(patsubst $(BUILDDIR)/%,build/%,$(HDD)) not found, run 'make hdd'" >&2; exit 1; }
	@$(ECHO) "  KERNEL  " $(patsubst $(BUILDDIR)/%,build/%,$(HDD))
	@$(SRCDIR)/mk/hdd-kernel.sh $(HDD) $(KERNEL) $(SRCDIR)/boot/limine-hdd.conf

hdd-update: hdd-kernel initrd
	@$(ECHO) "  UPDATE  " $(patsubst $(BUILDDIR)/%,build/%,$(HDD))
	@$(SRCDIR)/mk/hdd-update.sh $(HDD) $(ROOTFS)

run: kernel initrd
	$(QEMU) -kernel $(RUN_KERNEL) -initrd "$(INITRD)" -append earlycon=ttyS0 \
		-m 256 -serial mon:stdio -display none

run-iso: iso
	$(QEMU) -cdrom $(ISO) -m 256 -serial stdio

run-hdd:
	@if [ -f $(HDD) ]; then $(MAKE) hdd-update; else $(MAKE) hdd; fi
	$(QEMU) -hda $(HDD) -m 256 -serial stdio

#
# The quick start: the kernel with the host's gcc (tests/host/; aarch64's
# is clang already) and start/init.c as /init, no C library: nothing to
# build first
#

MIN_BUILD  := $(SRCDIR)/build/min-$(ARCH)$(if $(filter 1,$(DEBUG)),-debug)
MIN_CONFIG := $(if $(filter aarch64,$(ARCH)),$(CONFIG),../tests/host/$(ARCH)-pc)
MIN_CC     := $(if $(filter aarch64,$(ARCH)),clang --target=aarch64-none-elf -fuse-ld=lld,gcc -no-pie $(if $(filter i386,$(ARCH)),-m32))
MIN_KERNEL := $(MIN_BUILD)/kernel/kernel-$(VERSION).$(ARCH)$(if $(filter y,$(KERNEL_BINARY)),.img)

min:
	@$(MAKE) kernel CONFIG=$(MIN_CONFIG) BUILDDIR=$(MIN_BUILD)
	@$(ECHO) "  CC      " $(patsubst $(SRCDIR)/%,%,$(MIN_BUILD))/root/init
	@$(MKDIR) $(MIN_BUILD)/root/dev $(MIN_BUILD)/root/proc
	@$(MIN_CC) -static -nostdlib -ffreestanding -fno-builtin -fno-pic \
		-fno-stack-protector -O1 -Wall -I$(SRCDIR)/include/uapi -Wl,--build-id=none \
		-o $(MIN_BUILD)/root/init $(SRCDIR)/start/init.c
	@$(ECHO) "  CPIO    " $(patsubst $(SRCDIR)/%,%,$(MIN_BUILD))/initrd.img
	@cd $(MIN_BUILD)/root && find . | cpio -o -H bin -R 0:0 --quiet > $(MIN_BUILD)/initrd.img

run-min: min
	$(QEMU) -kernel $(MIN_KERNEL) -initrd $(MIN_BUILD)/initrd.img -append earlycon=ttyS0 \
		-m 256 -serial mon:stdio -display none

# QEMU stopped before the first instruction, its gdb server on port 1234
# (-s -S); the kernel's ELF file has the symbols and lines
debug:
	@$(MAKE) min DEBUG=1
	@echo "  QEMU waits for gdb. In another terminal:"
	@echo "    gdb -x mk/gdbinit $(patsubst $(SRCDIR)/%,%,$(MIN_BUILD)-debug/kernel/kernel-$(VERSION).$(ARCH))"
	$(QEMU) -kernel $(MIN_BUILD)-debug/kernel/kernel-$(VERSION).$(ARCH)$(if $(filter y,$(KERNEL_BINARY)),.img) \
		-initrd $(MIN_BUILD)-debug/initrd.img -append earlycon=ttyS0 \
		-m 256 -serial mon:stdio -display none -s -S

test:
	@$(SRCDIR)/tests/run.sh -a $(ARCH)

# the kernel's documents and code reference, from the comments in its
# sources
DOCS := $(SRCDIR)/build/docs/kernel
DOCS_PORT ?= 8000

docs:
	@command -v doxygen >/dev/null || { echo "error: doxygen not found" >&2; exit 1; }
	@mkdir -p $(DOCS)
	@$(ECHO) "  DOXYGEN " build/docs/kernel/api
	@cd $(SRCDIR)/docs/kernel && DOXYGEN_OUTPUT=$(DOCS) doxygen Doxyfile
	@echo "  $$(grep -c . $(DOCS)/warnings.log || true) warnings, in build/docs/kernel/warnings.log"

docs-serve: docs
	@echo "  the docs on http://localhost:$(DOCS_PORT)/ (Ctrl-C stops)"
	@python3 -m http.server --bind 127.0.0.1 --directory $(DOCS)/api $(DOCS_PORT)

clean:
	@$(RM) -r $(BUILDDIR)/kernel $(BUILDDIR)/userland $(ROOTFS) $(BUILDDIR)/iso \
		$(INITRD) $(ISO) $(BUILDDIR)/contrib $(BUILDDIR)/ports $(BUILDDIR)/staging

distclean:
	@$(RM) -r $(BUILDDIR)
