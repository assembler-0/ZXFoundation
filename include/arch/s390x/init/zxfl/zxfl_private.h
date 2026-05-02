// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl_private.h
//
/// @brief ZXFL loader-private definitions.
///
///        THIS FILE MUST NOT BE INCLUDED BY THE KERNEL.
///        It contains secrets used for the bidirectional lock:
///          - ZXFL_SEED: binding token seed (loader→kernel check)
///          - ZXFL_LOCK_*: kernel-lock constants (kernel→loader check)
///
///        Both sets of constants must be distributed out-of-band to the
///        kernel build.  The kernel uses them to:
///          1. Verify binding_token (proves it was booted by ZXFL).
///          2. Embed the correct lock word halves in its linker script
///             (proves ZXFL is loading an authentic ZXFoundation kernel).

#ifndef ZXFOUNDATION_ZXFL_PRIVATE_H
#define ZXFOUNDATION_ZXFL_PRIVATE_H

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// Loader → kernel: binding token
// ---------------------------------------------------------------------------

/// @brief Binding seed.  Change to re-key the pair.  Keep secret.
#define ZXFL_SEED               UINT64_C(0xA5F0C3E1B2D49687)

/// @brief Compute binding token: ZXFL_SEED ^ stfle_fac[0] ^ ipl_schid.
#define ZXFL_COMPUTE_TOKEN(stfle0, schid) \
    (ZXFL_SEED ^ (uint64_t)(stfle0) ^ (uint64_t)(schid))

// ---------------------------------------------------------------------------
// Mutual authentication handshake
//
// The loader calls the kernel's handshake stub at a fixed offset from the
// kernel load base before jumping to the real entry point.
//
// Protocol (all values in R2/R3):
//   Loader → stub:  R2 = ZXFL_HS_CHALLENGE ^ binding_token
//   Stub  → loader: R2 = ZXFL_HS_RESPONSE  ^ binding_token  (via R14 return)
//
// The stub is placed at kernel_base + ZXFL_HS_OFFSET by the linker script.
// This offset is secret — not in any ELF header or symbol table.
// ---------------------------------------------------------------------------
#define ZXFL_HS_OFFSET          0x0UL
#define ZXFL_HS_CHALLENGE       UINT64_C(0xFEEDFACECAFEBABE)
#define ZXFL_HS_RESPONSE        UINT64_C(0xDEADBEEF0BADF00D)

// ---------------------------------------------------------------------------
// Opaque stack frame — written by loader below kernel_stack_top.
// The kernel validates this before calling zxfoundation_global_initialize.
// Layout (64 bytes total, at kernel_stack_top - 64):
//   +0:  uint64_t magic_a   = ZXFL_FRAME_MAGIC_A
//   +8:  uint64_t magic_b   = ZXFL_FRAME_MAGIC_B ^ binding_token
//   +16: uint64_t reserved[6] = 0
// An outsider's kernel has no ZXFL_FRAME_MAGIC_A/B and cannot validate.
// ---------------------------------------------------------------------------
#define ZXFL_FRAME_MAGIC_A      UINT64_C(0xC0FFEE00FACADE42)
#define ZXFL_FRAME_MAGIC_B      UINT64_C(0x1337BABE0DDBA115)
#define ZXFL_FRAME_SIZE         64U

// ---------------------------------------------------------------------------
// Kernel → loader: split lock word
//
// The kernel linker script embeds two 32-bit values in .zxfl_lock:
//   [kernel_base + ZXFL_LOCK_OFFSET + 0x000]: ZXFL_LOCK_EMBED_HI
//   [kernel_base + ZXFL_LOCK_OFFSET + 0x004]: ZXFL_LOCK_SENTINEL (0x5A58464C)
//   [kernel_base + ZXFL_LOCK_OFFSET + 0x1000]: ZXFL_LOCK_EMBED_LO
//
// The loader reads both after ELF load, reconstructs the 64-bit word,
// XORs with ZXFL_LOCK_MASK, and checks == ZXFL_LOCK_EXPECTED.
//
// Derivation (keep these three values secret):
//   ZXFL_LOCK_MASK     = 0x3C1E0F8704B2D596
//   ZXFL_LOCK_EXPECTED = 0xF0A5C3B2E1D49687
//   embed = EXPECTED ^ MASK = 0xCCBBCC35E5664311
//   HI = embed >> 32        = 0xCCBBCC35
//   LO = embed & 0xFFFFFFFF = 0xE5664311
// ---------------------------------------------------------------------------

#define ZXFL_LOCK_MASK          UINT64_C(0x3C1E0F8704B2D596)
#define ZXFL_LOCK_EXPECTED      UINT64_C(0xF0A5C3B2E1D49687)
#define ZXFL_LOCK_EMBED_HI      0xCCBBCC35U
#define ZXFL_LOCK_EMBED_LO      0xE5664311U

// ---------------------------------------------------------------------------
// Build timestamp
// ---------------------------------------------------------------------------
#define _ZX_CH2(s, i)   ((uint32_t)((s)[i] - '0') * 10U + (uint32_t)((s)[i+1] - '0'))
#define ZXFL_BUILD_TS   ((_ZX_CH2(__TIME__, 0) << 24) | \
                         (_ZX_CH2(__TIME__, 3) << 16) | \
                         (_ZX_CH2(__TIME__, 6) <<  8) | \
                          0x5AU)

#endif /* ZXFOUNDATION_ZXFL_PRIVATE_H */
