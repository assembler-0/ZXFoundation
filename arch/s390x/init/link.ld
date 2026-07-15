/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld — ZXFoundation Kernel Linker Script */

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
BYTE = 8;

SECTIONS {
    . = nucleus_vma_base;
    __zx_vma_start = .;

    .text.handshake : ALIGN(BYTE) {
        KEEP(*(.text.handshake))
    } :hs_seg

    .text.entry : ALIGN(BYTE) {
        KEEP(*(.text.__zx_start_kernel64))
    } :entry_seg

    .text : ALIGN(BYTE) {
        *(.text .text.*)
    } :nucleus_text

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :nucleus_ro

    .ex_table : ALIGN(8) {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    } :nucleus_ro

    .ctors ALIGN(BYTE) :
    {
        __ctors_start = .;
        KEEP(*(.ctors*))
        . = ALIGN(BYTE);
        __ctors_end = .;
    } :nucleus_ro

	.dtors ALIGN(BYTE) :
	{
	    __dtors_start = .;
	    KEEP(*(.dtors*))
   	    . = ALIGN(BYTE);
	    __dtors_end = .;
	} :nucleus_ro

    .init_array ALIGN(BYTE) :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        PROVIDE_HIDDEN(__init_array_end   = .);
    } :nucleus_ro

    .fini_array ALIGN(BYTE) :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .fini_array))
        PROVIDE_HIDDEN(__fini_array_end   = .);
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
        . = ALIGN(BYTE);
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

   /DISCARD/ : {
       *(.eh_frame)
       *(.eh_frame_hdr)
   }
}
