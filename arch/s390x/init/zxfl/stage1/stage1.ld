/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/zxfl/stage1/stage1.ld */

/*
 * core.zxfoundationloader00.sys layout:
 *   0x0000 - 0x0017 : IPL PSW + padding (.text.ipl, 24 bytes)
 *   0x0018 - 0x0057 : unused (padding to 0x58)
 *   0x0058 - ...    : stage1_entry + C code + rodata + data
 *   ...  - 0x1FFF   : fit within 8192 bytes total
 */

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

    ASSERT(LOADADDR(.bss) <= 0x2000, "stage1: code+data exceeds 8K")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
