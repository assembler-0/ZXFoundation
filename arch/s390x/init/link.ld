/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld — ZXFoundation Kernel Linker Script */

ENTRY(__zx_start_kernel64)

PHDRS {
    hs_seg        PT_LOAD FLAGS(0x00400005);  /* ZXVL_PFLAGS_HS:    PF_X|PF_R + bit22 */
    entry_seg     PT_LOAD FLAGS(0x00800005);  /* ZXVL_PFLAGS_ENTRY: PF_X|PF_R + bit23 */
    nucleus_text  PT_LOAD FLAGS(0x00000005);  /* PF_X|PF_R  — kernel .text             */
    nucleus_ro    PT_LOAD FLAGS(0x00000004);  /* PF_R       — kernel .rodata            */
    nucleus_data  PT_LOAD FLAGS(0x00000006);  /* PF_R|PF_W  — kernel .data             */
    nucleus_bss   PT_LOAD FLAGS(0x00000006);  /* PF_R|PF_W  — kernel .bss              */
    lock_seg      PT_LOAD FLAGS(0x00100006);  /* ZXVL_PFLAGS_LOCK:  PF_R|PF_W + bit20 */
    checksums_seg PT_LOAD FLAGS(0x00200004);  /* ZXVL_PFLAGS_CKSUM: PF_R + bit21      */
}

nucleus_vma_base = 0xFFFF800000100000;

SECTIONS {
    . = nucleus_vma_base;
    __zx_vma_start = .;

    .text.handshake : ALIGN(8) {
        KEEP(*(.text.handshake))
    } :hs_seg

    .text.entry : ALIGN(8) {
        KEEP(*(.text.__zx_start_kernel64))
    } :entry_seg

    .text : ALIGN(8) {
        *(.text .text.*)
    } :nucleus_text

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :nucleus_ro

    .data : ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :nucleus_data

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(8);
        __bss_end = .;
    } :nucleus_bss

    .zxfl_lock : ALIGN(4096) {
        LONG(0xCCBBCC35)
        LONG(0x5A58464C)
        . += 0x1000 - 8;
        LONG(0xE5664311)
    } :lock_seg

    .zxvl_checksums : ALIGN(4096) {
        KEEP(*(.zxvl_checksums))
    } :checksums_seg

    __zx_vma_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.GNU-stack) }
}
