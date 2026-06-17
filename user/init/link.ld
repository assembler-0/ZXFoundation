/* SPDX-License-Identifier: Apache-2.0 */
/* User init linker script */

OUTPUT_FORMAT("elf64-s390");
OUTPUT_ARCH(s390:64-bit)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.note.* .comment .eh_frame .eh_frame_hdr)
    }
}
