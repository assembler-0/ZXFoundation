/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld - Robust Higher-Half Linker Script */

ENTRY(__zx_start_kernel64)

PHDRS {
    nucleus PT_LOAD FLAGS(7); /* RWE */
}

SECTIONS {
    /* 1. PHYSICAL ANCHOR: The entire image starts at 0x100000 */
    
    /* HANDSHAKE STUB at Offset 0x0 */
    .text.handshake 0xFFFF800000100000 : AT(0x100000) {
        KEEP(*(.text.handshake))
    } :nucleus

    /* BOOTSTRAP CODE at Offset 0x400 */
    .text 0xFFFF800000100400 : AT(0x100400) {
        KEEP(*(.text.__zx_start_kernel64))
        *(.text .text.*)
    } :nucleus

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    } :nucleus

    .data : ALIGN(8) {
        *(.data .data.*)
    } :nucleus

    /* SECURITY LOCK at Offset 0x70000 */
    .zxfl_lock 0xFFFF800000170000 : AT(0x100000 + 0x70000) {
        KEEP(*(.zxfl_lock))
        LONG(0xCCBBCC35)
        LONG(0x5A58464C)
        . = . + 0x1000 - 8;
        LONG(0xE5664311)
    } :nucleus

    /* BSS at the very end */
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :nucleus

    __kernel_virt_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.GNU-stack) }
}
