/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld - ZXFoundation Higher-Half Linker Script
 *
 * VMA: 0xFFFF800000100000  (HHDM offset + 1 MB physical load address)
 * LMA: 0x100000            (physical load address, used by the ELF loader)
 *
 * The loader reads p_paddr (LMA) to place segments in physical memory,
 * then enables DAT and jumps to e_entry (VMA).  All kernel code and data
 * references are resolved at VMA — the kernel runs entirely in virtual
 * memory from the first instruction.
 */

ENTRY(__zx_start_kernel64)

PHDRS {
    nucleus PT_LOAD FLAGS(7); /* RWE */
}

SECTIONS {
    /* PHYSICAL ANCHOR: The entire image loads at physical 0x100000 (1 MB).
     * VMA = CONFIG_KERNEL_VIRT_OFFSET + physical offset.
     * 0xFFFF800000000000 + 0x100000 = 0xFFFF800000100000 */

    /* HANDSHAKE STUB at physical offset 0x0 (kernel_base + 0x0).
     * Called by the loader at physical address before DAT is on. */
    .text.handshake 0xFFFF800000100000 : AT(0x100000) {
        KEEP(*(.text.handshake))
    } :nucleus

    /* KERNEL ENTRY at physical offset 0x400 */
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

    /* SECURITY LOCK at physical offset 0x70000 */
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
