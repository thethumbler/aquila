#
# Kernel build
#
# Non-recursive: walks the Build.mk files of the architecture and of the
# top level directories, compiles every object into $(KBUILD) and links
# them in that order (built-in modules are initialized in link order).
#
# Build.mk files list objects and subdirectories:
#
#   obj-y  += foo.o             obj-$(CONFIG_VAR) += bar.o
#   dirs-y += sub/              dirs-$(CONFIG_VAR) += baz/
#

KSRC   := $(SRCDIR)/kernel
KBUILD ?= $(BUILDDIR)/kernel
KERNEL := $(KBUILD)/kernel-$(VERSION).$(ARCH)

LDSCRIPT := $(KSRC)/arch/$(ARCH_DIR)/kernel.$(ARCH).ld

INCLUDES := \
	-I$(SRCDIR)/include/uapi \
	-I$(KSRC)/arch/$(ARCH_DIR)/platform/$(PLATFORM_DIR)/include \
	-I$(KSRC)/arch/$(ARCH_DIR)/include \
	-I$(KSRC)/include \
	-I$(KSRC)

# the configuration's options set to y, to C as CONFIG_<option> (as
# Linux's): code that only some configurations have is #ifdef'd on them
KCONFIG := $(foreach v,$(filter ARCH_% PLATFORM_% DEV_% FS_% NET_% INITRAMFS_% FBDEV_% X86_%,$(.VARIABLES)),$(if $(filter y,$($(v))),-DCONFIG_$(v)))

KCFLAGS  := $(CFLAGS) $(INCLUDES) $(KCONFIG) -MMD -MP
KASFLAGS := $(ASFLAGS) $(INCLUDES) $(KCONFIG) -MMD -MP

# DEBUG=1: debugging information, and only the optimizations that keep
# the code close to the source, for gdb (make debug). Build it in a
# directory of its own: flags alone don't make objects be rebuilt.
ifeq ($(DEBUG),1)
KCFLAGS  += -g -Og
KASFLAGS += -g
endif

#
# Collect objects
#

objs :=

# $(call walk,dir/) - add the objects of dir/Build.mk, then of its subdirectories
define walk
obj-y :=
dirs-y :=
include $(KSRC)/$(1)Build.mk
objs += $$(addprefix $(1),$$(obj-y))
dirs_$(1) := $$(dirs-y)
$$(foreach d,$$(dirs_$(1)),$$(eval $$(call walk,$(1)$$(d))))
endef

$(eval $(call walk,arch/$(ARCH_DIR)/))
$(foreach d,core/ fs/ dev/ sys/ mm/ net/,$(eval $(call walk,$(d))))

OBJS := $(addprefix $(KBUILD)/,$(objs))

#
# Rules
#

.PHONY: all install clean

all: $(KERNEL) $(if $(filter y,$(KERNEL_BINARY)),$(KERNEL).img)

$(KERNEL): $(OBJS) $(LDSCRIPT)
	@$(ECHO) "  LD      " $(patsubst $(BUILDDIR)/%,%,$@)
	@$(LD) $(LDFLAGS) -T $(LDSCRIPT) $(OBJS) $(LDLIBS) -o $@

# the kernel's memory image, flat, for boot loaders that take one
# (KERNEL_BINARY: aarch64's, Linux's arm64 Image)
$(KERNEL).img: $(KERNEL)
	@$(ECHO) "  OBJCOPY " $(patsubst $(BUILDDIR)/%,%,$@)
	@$(OBJCOPY) -O binary $< $@

# sources may include headers next to them with <...>
$(KBUILD)/%.o: $(KSRC)/%.c
	@$(MKDIR) $(@D)
	@$(ECHO) "  CC      " kernel/$*.o
	@$(CC) $(KCFLAGS) -I$(<D) -c $< -o $@

$(KBUILD)/%.o: $(KSRC)/%.S
	@$(MKDIR) $(@D)
	@$(ECHO) "  AS      " kernel/$*.o
	@$(AS) $(KASFLAGS) -I$(<D) -c $< -o $@

install: $(KERNEL)
	@$(ECHO) "  INSTALL " $(DESTDIR)/boot/kernel
	@$(MKDIR) $(DESTDIR)/boot
	@$(CP) $(KERNEL) $(DESTDIR)/boot/kernel

clean:
	@$(RM) -r $(KBUILD)

-include $(OBJS:.o=.d)
