/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/zxfl/zxfl.ld - ZXFoundation Loader script */

ENTRY(__zx_ipl_entry)

SECTIONS {
    . = 0x0;

    .text.ipl : {
        KEEP(*(.text.ipl))
    }

    ASSERT(. <= 0x18, "ERROR: IPL1 record exceeds 24 bytes (0x18)")

    . = 0x200;
    .text : {
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

    .bss : ALIGN(8) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }

    ASSERT(. <= 0x10000, "ERROR: ZXFL Bootloader exceeds 64KB max size")

    /DISCARD/ : {
        *(.comment)
        *(.note.GNU-stack)
        *(.eh_frame)
    }
}
