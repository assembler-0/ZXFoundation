/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/stage1/stage1.ld - ZXFoundation linker script for stage1 */

ENTRY(__zx_ipl1_record)

SECTIONS {
    . = 0x0;

    .text.ipl1 : {
        KEEP(*(.text.ipl1))
    }

    ASSERT(. == 0x18, "ERROR: IPL1 record must be exactly 24 bytes (0x18)")

    .text.loader : {
        KEEP(*(.text.loader))
    }

    ASSERT(. <= 0x1000, "ERROR: Stage1 loader exceeds 4KB")

    /DISCARD/ : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.bss)
        *(.bss.*)
        *(COMMON)
        *(.comment)
        *(.note.GNU-stack)
        *(.eh_frame)
    }
}
