/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld — ZXFoundation Kernel Linker Script */
ENTRY(__zx_start_kernel64)

PHDRS {
    nucleus      PT_LOAD FLAGS(7);
    checksums_seg PT_LOAD FLAGS(4);
}

nucleus_vma_base = 0xFFFF800000100000;

SECTIONS {
    . = nucleus_vma_base;
    __zx_vma_start = .;
    
    .text.handshake : {
        KEEP(*(.text.handshake))
    } :nucleus

    .text nucleus_vma_base + 0x400 : {
        KEEP(*(.text.__zx_start_kernel64))
        *(.text .text.*)
    } :nucleus

    .rodata : ALIGN(8) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :nucleus

    .data : ALIGN(8) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :nucleus

    .zxfl_lock nucleus_vma_base + 0x70000 : {
        KEEP(*(.zxfl_lock))
        LONG(0xCCBBCC35)
        LONG(0x5A58464C)
        . = . + 0x1000 - 8;
        LONG(0xE5664311)
    } :nucleus

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :nucleus

    .zxvl_checksums nucleus_vma_base + 0x80000 : {
        KEEP(*(.zxvl_checksums))
    } :checksums_seg

    __zx_vma_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.GNU-stack) }
}
