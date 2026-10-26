/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld — ZXFoundation Kernel Linker Script */

OUTPUT_FORMAT("elf64-s390", "elf64-s390", "elf64-s390")
OUTPUT_ARCH(s390:64-bit)
ENTRY(__zx_start_kernel64)

PHDRS {
    hs_seg        PT_LOAD FLAGS(0x00400005);  /* ZXVL_PFLAGS_HS:    PF_X|PF_R + bit22 */
    entry_seg     PT_LOAD FLAGS(0x00800005);  /* ZXVL_PFLAGS_ENTRY: PF_X|PF_R + bit23 */
    nucleus_text  PT_LOAD FLAGS(0x00000005);  /* PF_X|PF_R  — kernel .text            */
    nucleus_ro    PT_LOAD FLAGS(0x00000004);  /* PF_R       — kernel .rodata           */
    nucleus_data  PT_LOAD FLAGS(0x00000006);  /* PF_R|PF_W  — kernel .data             */
    nucleus_bss   PT_LOAD FLAGS(0x00000006);  /* PF_R|PF_W  — kernel .bss              */
    lock_seg      PT_LOAD FLAGS(0x00100006);  /* ZXVL_PFLAGS_LOCK:  PF_R|PF_W + bit20  */
    checksums_seg PT_LOAD FLAGS(0x00200004);  /* ZXVL_PFLAGS_CKSUM: PF_R + bit21       */
}

nucleus_vma_base = 0xFFFF800000100000;
PAGE = 4096;

SECTIONS {
    . = nucleus_vma_base;
    __zx_vma_start = .;

    . = ALIGN(PAGE);
    .text.handshake : ALIGN(PAGE) {
        KEEP(*(.text.handshake))
        . = ALIGN(PAGE);
    } :hs_seg

    . = ALIGN(PAGE);
    .text.entry : ALIGN(PAGE) {
        KEEP(*(.text.__zx_start_kernel64))
        . = ALIGN(PAGE);
    } :entry_seg

    . = ALIGN(PAGE);
    .text : ALIGN(PAGE) {
        *(.text .text.*)
        . = ALIGN(PAGE);
    } :nucleus_text

    . = ALIGN(PAGE);
    .rodata : ALIGN(PAGE) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;

        . = ALIGN(8);
        __zinit_start = .;
        KEEP(*(SORT_BY_NAME(.zinit_data.*)))
        __zinit_end = .;
    } :nucleus_ro

    .ex_table : ALIGN(8) {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    } :nucleus_ro

    .ctors ALIGN(8) :
    {
        __ctors_start = .;
        KEEP(*(.ctors*))
        . = ALIGN(8);
        __ctors_end = .;
    } :nucleus_ro

    .dtors ALIGN(8) :
    {
        __dtors_start = .;
        KEEP(*(.dtors*))
        . = ALIGN(8);
        __dtors_end = .;
    } :nucleus_ro

    .init_array ALIGN(8) :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        PROVIDE_HIDDEN(__init_array_end   = .);
    } :nucleus_ro

    .fini_array ALIGN(8) :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .fini_array))
        PROVIDE_HIDDEN(__fini_array_end   = .);
    } :nucleus_ro

    . = ALIGN(PAGE);
    . = ALIGN(PAGE);
    .data : ALIGN(PAGE) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
        . = ALIGN(PAGE);
    } :nucleus_data

    . = ALIGN(PAGE);
    .bss : ALIGN(PAGE) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.bss.boot_stack .bss.boot_stack.*)
        . = ALIGN(PAGE);
        __bss_end = .;
    } :nucleus_bss

    . = ALIGN(PAGE);
    .zxfl_lock : ALIGN(PAGE) {
        LONG(0xCCBBCC35)
        LONG(0x5A58464C)
        . += 0x1000 - 8;
        LONG(0xE5664311)
        . = ALIGN(PAGE);
    } :lock_seg

    . = ALIGN(PAGE);
    .zxvl_checksums : ALIGN(PAGE) {
        KEEP(*(.zxvl_checksums))
        . = ALIGN(PAGE);
    } :checksums_seg

    __zx_vma_end = .;

   /DISCARD/ : {
       *(.eh_frame)
       *(.eh_frame_hdr)
    }
}
