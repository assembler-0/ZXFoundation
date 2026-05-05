/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld — ZXFoundation Kernel Linker Script */
ENTRY(__zx_start_kernel64)

PHDRS {
    nucleus      PT_LOAD FLAGS(7);
    checksums_seg PT_LOAD FLAGS(4);
}

SECTIONS {
    .text.handshake 0xFFFF800000100000 : {
        KEEP(*(.text.handshake))
    } :nucleus

    .text 0xFFFF800000100400 : {
        KEEP(*(.text.__zx_start_kernel64))
        *(.text .text.*)
    } :nucleus

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    } :nucleus

    .data : ALIGN(8) {
        *(.data .data.*)
    } :nucleus

    .zxfl_lock 0xFFFF800000170000 : {
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

    /*
     * .zxvl_checksums lives in its own PT_LOAD (checksums_seg) so that
     * gen_checksums can identify and skip it during hashing.  It is
     * placed at a fixed virtual address (load_min + 0x80000 = 0x180000
     * physical) matching ZXVL_CKSUM_TABLE_OFFSET.
     *
     * The section is populated by the zxvl_cksum_anchor symbol defined
     * in arch/s390x/init/zxvl_anchor.c, which reserves exactly
     * sizeof(zxvl_checksum_table_t) bytes.  gen_checksums patches the
     * real digests into this region after linking.
     */
    .zxvl_checksums 0xFFFF800000180000 : {
        KEEP(*(.zxvl_checksums))
    } :checksums_seg

    __kernel_virt_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.GNU-stack) }
}
