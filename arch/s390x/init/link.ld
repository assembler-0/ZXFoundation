/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld - ZXFoundation linker script for s390x */

ENTRY(__zx_start_kernel31)

SECTIONS {
    /* Kernel is loaded at 0x10000 by IPL READ CCW */
    . = 0x10000;

    /* Entry point at 0x10000 (file offset 0x0) */
    /* NO PADDING - kernel code starts immediately at load address */
    .text : {
        /* Stage2 31-bit entry point comes first, then 64-bit */
        KEEP(*(.text.__zx_start_kernel31))
        KEEP(*(.text.__zx_start_kernel64))
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

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}
