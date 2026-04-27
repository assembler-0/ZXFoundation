/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/boot/link.ld - ZXFoundation linker script for s390x
 *
 * Load address 0x10000: the s390-ccw-virtio firmware IPL loader jumps to
 * the physical start of the loaded image, so _start must be first.
 */

ENTRY(_start)

SECTIONS {
    . = 0x10000;

    /* -------------------------------------------------------------------
     * .text - executable code
     * _start must be the very first thing in the image.
     * ------------------------------------------------------------------- */
    .text : {
        KEEP(*(.text._start))
        *(.text)
        *(.text.*)
    }

    /* -------------------------------------------------------------------
     * .rodata - read-only data (string literals, const globals, etc.)
     * ------------------------------------------------------------------- */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /* -------------------------------------------------------------------
     * .data - initialised read-write data
     * ------------------------------------------------------------------- */
    .data : {
        *(.data)
        *(.data.*)
    }

    /* -------------------------------------------------------------------
     * .bss - zero-initialised data
     *
     * __bss_start and __bss_end are used by entry.S to zero this region
     * before calling into C. The firmware does NOT guarantee zeroed memory.
     * ------------------------------------------------------------------- */
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }
}
