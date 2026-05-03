/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/zxfl/stage1/stage1.ld */

ENTRY(__zx_ipl_entry)

SECTIONS
{
    . = 0x0;
    .text.ipl : {
        KEEP(*(.text.ipl))
    }

    . = 0x58;
    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_stop = .;
    }

    ASSERT(LOADADDR(.bss) <= 0x3000, "stage1: code+data exceeds 12K")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
