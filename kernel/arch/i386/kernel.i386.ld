OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
OUTPUT(kernel.elf)

SECTIONS {
    LMA = 0x100000;
    . = LMA;

    kernel_start = .;

    /* the boot code (arch/i386/boot/), run before paging, at its
     * physical address; not other objects with "boot" in their name */
    .boot.text : ALIGN(0x1000) {
        */arch/i386/boot/*.o(.text*)
        */arch/i386/boot/*.o(.rodata*)
    }

    .boot.data : ALIGN(0x1000) {
        */arch/i386/boot/*.o(.data)
    }

    .boot.bss (NOLOAD) : ALIGN(0x1000) {
        */arch/i386/boot/*.o(*)
    }

    boot_end = .;

    _VMA = 0xC0000000;
    . += _VMA;

    .text : AT(ADDR(.text) - _VMA) ALIGN(0x1000) {
        *(.text*)
        *(.rodata*)
    }

    .__minit : AT(ADDR(.__minit) - _VMA) ALIGN(0x1000) {
        __minit = .;
        *(.__minit*)
        __minit_end = .;
    }
    
    .data : AT(ADDR(.data) - _VMA) ALIGN(0x1000) {
        *(.data)
    }
    
    .bss : AT(ADDR(.bss) - _VMA) ALIGN(0x1000) {
        *(.bss)
    }

    .rel.plt : AT(ADDR(.rel.plt) - _VMA) ALIGN(0x1000) {
        *(.rel.plt)
    }

    .strtab : AT(ADDR(.strtab) - _VMA) ALIGN(0x1000) {
        *(.strtab)
    }

    .shstrtab : AT(ADDR(.shstrtab) - _VMA) ALIGN(0x1000) {
        *(.shstrtab)
    }
    
    kernel_end = . - _VMA;

    /* The boot loader loads the file from kernel_start to kernel_load_end
     * as one piece, so the sections must follow each other in the file as
     * in memory. A section aligned to more than a page would make ld align
     * its segment's place in the file too, and move it. */
    ASSERT(ALIGNOF(.data) <= 0x1000 && ALIGNOF(.bss) <= 0x1000,
           "a section aligned to more than a page: the kernel can't be loaded")

    /* debugging information (DEBUG=1, make debug): in the file for gdb,
     * not loaded; after kernel_end, which their address 0 would move */
    .debug_aranges     0 : { *(.debug_aranges) }
    .debug_info        0 : { *(.debug_info) }
    .debug_abbrev      0 : { *(.debug_abbrev) }
    .debug_line        0 : { *(.debug_line) }
    .debug_line_str    0 : { *(.debug_line_str) }
    .debug_str         0 : { *(.debug_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_addr        0 : { *(.debug_addr) }
    .debug_frame       0 : { *(.debug_frame) }
    .debug_loclists    0 : { *(.debug_loclists) }
    .debug_rnglists    0 : { *(.debug_rnglists) }
    .debug_loc         0 : { *(.debug_loc) }
    .debug_ranges      0 : { *(.debug_ranges) }

    /DISCARD/ : {
        *(.*)
    }
}
