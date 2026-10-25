#
# i386-pc compiled with tcc (TCC=<tcc>, default i386-tcc: tcc's i386
# compiler, make cross-i386 in its sources), the assembly and the link
# with the host's gcc and ld, as tcc's assembler doesn't take all of it
# and ld takes the linker script:
#
#   make kernel CONFIG=../tests/host/i386-pc-tcc
#

include $(SRCDIR)/configs/i386-pc.mk

TCC ?= i386-tcc

CC := $(TCC)
CFLAGS := -std=gnu11 -nostdlib -ffreestanding -Wall

AS := gcc
ASFLAGS := -m32 -ffreestanding -nostdlib -fno-pic -fno-pie

LD := ld

# tcc's runtime library, its libgcc: atomics, __builtin_clzl(); in
# its directory, a cross compiler's named after its target
TCC_DIR := $(shell $(TCC) -print-search-dirs | sed -n 's/^install: //p')
LDLIBS := $(firstword $(wildcard $(TCC_DIR)/i386-libtcc1.a $(TCC_DIR)/libtcc1.a))
