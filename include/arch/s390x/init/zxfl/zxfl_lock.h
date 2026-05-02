// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl_lock.h
//
/// @brief ZXFL kernel-lock mechanism.
///
///        Ensures ONLY ZXFoundationLoader can load ZXFoundation.
///
///        The kernel linker script places a lock structure at a fixed offset
///        (ZXFL_LOCK_OFFSET) from the kernel load base (lowest PT_LOAD paddr).
///        The structure layout:
///
///            [offset + 0x000] uint32_t lock_hi       = ZXFL_LOCK_EMBED_HI
///            [offset + 0x004] uint32_t sentinel      = ZXFL_LOCK_SENTINEL
///            [offset + 0x008] uint8_t  _pad[0xFF8]   (fills to 4 KB boundary)
///            [offset + 0x1000] uint32_t lock_lo      = ZXFL_LOCK_EMBED_LO
///
///        The 4 KB gap between hi and lo makes the lock non-trivially
///        discoverable by a linear scan — an outsider must know the layout.
///
///        The loader verifies after ELF load:
///            1. sentinel == ZXFL_LOCK_SENTINEL  (guards against zero-fill)
///            2. (lock_hi << 32 | lock_lo) ^ ZXFL_LOCK_MASK == ZXFL_LOCK_EXPECTED
///
///        ZXFL_LOCK_MASK and ZXFL_LOCK_EXPECTED are in zxfl_private.h only.
///
///        KERNEL LINKER SCRIPT FRAGMENT (arch/s390x/init/link.ld):
///        ----------------------------------------------------------
///        .zxfl_lock ZXFL_LOCK_BASE : {
///            LONG(ZXFL_LOCK_EMBED_HI)        /* lock_hi  */
///            LONG(0x5A58464C)                /* sentinel */
///            . = ALIGN(0x1000);
///            LONG(ZXFL_LOCK_EMBED_LO)        /* lock_lo  */
///            . = ALIGN(0x1000);
///        }
///        where ZXFL_LOCK_BASE = kernel_load_base + ZXFL_LOCK_OFFSET
///        ----------------------------------------------------------

#ifndef ZXFOUNDATION_ZXFL_LOCK_H
#define ZXFOUNDATION_ZXFL_LOCK_H

/// @brief Offset from kernel load base (0x10000) where the lock structure sits.
///        0x70000 → absolute address 0x80000. Must match link.ld.
#define ZXFL_LOCK_OFFSET        0x70000U

/// @brief Gap between lock_hi and lock_lo within the lock structure.
#define ZXFL_LOCK_GAP           0x1000U

/// @brief Sentinel value at [lock_base + 4].  ASCII "ZXFL".
#define ZXFL_LOCK_SENTINEL      0x5A58464CU

#endif /* ZXFOUNDATION_ZXFL_LOCK_H */
