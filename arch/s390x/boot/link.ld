/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/boot/link.ld - ZXFoundation linker script for s390x */

ENTRY(_start)

SECTIONS {
    . = 0x10000;

    .text : {
        KEEP(*(.text._start))
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }
}
