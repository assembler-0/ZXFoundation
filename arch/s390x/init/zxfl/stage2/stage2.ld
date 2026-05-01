/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/zxfl/stage2/stage2.ld */

OUTPUT_FORMAT("elf64-s390")
OUTPUT_ARCH(s390:64-bit)
ENTRY(stage2_entry)

SECTIONS
{
    . = 0x20000;

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

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
