#
# x86_64-pc compiled with tcc (TCC=<tcc>, default tcc), the assembly and
# the link with the host's gcc and ld, as tcc's assembler doesn't take all
# of it and ld takes the linker script:
#
#   make kernel CONFIG=../tests/host/x86_64-pc-tcc
#

include $(SRCDIR)/configs/x86_64-pc.mk

TCC ?= tcc

# no SSE: tcc saves the SSE registers in a variadic function otherwise,
# and the kernel doesn't touch them (they're user space's)
CC := $(TCC)
CFLAGS := -std=gnu11 -nostdlib -ffreestanding -mno-sse -Wall

AS := gcc
ASFLAGS := -m64 -ffreestanding -nostdlib -fno-pic -fno-pie

LD := ld

# tcc's runtime library, its libgcc: atomics, va_arg, __builtin_clzl(); in
# its directory, a cross compiler's named after its target
TCC_DIR := $(shell $(TCC) -print-search-dirs | sed -n 's/^install: //p')
LDLIBS := $(firstword $(wildcard $(TCC_DIR)/x86_64-libtcc1.a $(TCC_DIR)/libtcc1.a))
