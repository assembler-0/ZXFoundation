/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/init/link.ld - ZXFoundation linker script for s390x */

ENTRY(__zx_start_kernel64)

SECTIONS {
    . = 0x40000;

    /* Handshake stub at kernel_base + ZXFL_HS_OFFSET (0x0 = kernel_base itself).
     * Called by the loader before jumping to the real entry point at 0x40400. */
    .text.handshake : {
        KEEP(*(.text.handshake))
    }

    . = 0x40400;
    .text : {
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

    /*
     * ZXFL kernel-lock structure.
     * Fixed at 0x10000 + ZXFL_LOCK_OFFSET (0x70000) = 0x80000.
     * Must not overlap .text/.data/.bss — 0x80000 gives 448 KB of headroom
     * above the kernel load base, well beyond any foreseeable kernel size.
     *
     * Layout (loader reads these after ELF load):
     *   0x80000: lock_hi  = ZXFL_LOCK_EMBED_HI  (0xCCBBCC35)
     *   0x80004: sentinel = 0x5A58464C ("ZXFL")
     *   0x81000: lock_lo  = ZXFL_LOCK_EMBED_LO  (0xE5664311)
     *
     * Verify: (lock_hi << 32 | lock_lo) ^ ZXFL_LOCK_MASK == ZXFL_LOCK_EXPECTED
     * See include/arch/s390x/init/zxfl/zxvl_private.h for constants.
     * Re-key by updating ZXFL_LOCK_EMBED_HI/LO and the two LONG() values below.
     */
    . = 0xB0000;
    .zxfl_lock : {
        LONG(0xCCBBCC35)        /* lock_hi  = ZXFL_LOCK_EMBED_HI */
        LONG(0x5A58464C)        /* sentinel = ASCII "ZXFL"        */
        . = ALIGN(0x1000);
        LONG(0xE5664311)        /* lock_lo  = ZXFL_LOCK_EMBED_LO */
        . = ALIGN(0x1000);
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}
