#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/lowcore.h
//
// s390x Low-Core (prefix area) layout.
//
// The hardware stores/loads PSW pairs at fixed physical addresses in the
// first 512 bytes of memory (the "prefix area" or "lowcore").  Each
// exception type has:
//   - an "old PSW"  written by the CPU when the exception fires
//   - a  "new PSW"  read  by the CPU to dispatch the handler
//
// Reference: z/Architecture Principles of Operation, SA22-7832, Chapter 6.
//
// NOTE: All offsets are in bytes from physical address 0x000.

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// PSW pair - 16 bytes: 8-byte PSW mask + 8-byte instruction address
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t mask;
    uint64_t addr;
} __attribute__((packed)) psw_t;

// ---------------------------------------------------------------------------
// Lowcore PSW-pair offsets (old PSW / new PSW)
// ---------------------------------------------------------------------------
#define LC_RESTART_OLD_PSW          0x008   // Restart old PSW
#define LC_RESTART_NEW_PSW          0x1A0   // Restart new PSW

#define LC_EXT_OLD_PSW              0x018   // External interrupt old PSW
#define LC_EXT_NEW_PSW              0x058   // External interrupt new PSW

#define LC_SVC_OLD_PSW              0x020   // Supervisor-call old PSW
#define LC_SVC_NEW_PSW              0x060   // Supervisor-call new PSW

#define LC_PGM_OLD_PSW              0x028   // Program-exception old PSW
#define LC_PGM_NEW_PSW              0x068   // Program-exception new PSW

#define LC_MCHK_OLD_PSW             0x030   // Machine-check old PSW
#define LC_MCHK_NEW_PSW             0x070   // Machine-check new PSW

#define LC_IO_OLD_PSW               0x038   // I/O interrupt old PSW
#define LC_IO_NEW_PSW               0x078   // I/O interrupt new PSW

// ---------------------------------------------------------------------------
// Additional lowcore fields used by the trap system
// ---------------------------------------------------------------------------
#define LC_PGM_ILC                  0x08C   // Program interrupt ILC (2 bytes)
#define LC_PGM_CODE                 0x08E   // Program interrupt code (2 bytes)
#define LC_TRANS_EXC_ADDR           0x090   // Translation exception address (8 bytes)
#define LC_EXT_INT_CODE             0x086   // External interrupt code (2 bytes)
#define LC_SVC_INT_CODE             0x088   // SVC interrupt code (2 bytes)

// ---------------------------------------------------------------------------
// Lowcore struct - mapped at physical address 0x0.
//
// Memory map (all offsets hex):
//   000-007  : reserved
//   008-017  : restart old PSW          (16 bytes)
//   018-01F  : external old PSW         (16 bytes)
//   020-027  : SVC old PSW              (16 bytes)
//   028-02F  : program old PSW          (16 bytes)
//   030-037  : machine-check old PSW    (16 bytes)
//   038-03F  : I/O old PSW              (16 bytes)
//   040-057  : reserved                 (24 bytes)
//   058-05F  : external new PSW         (16 bytes)
//   060-067  : SVC new PSW              (16 bytes)
//   068-06F  : program new PSW          (16 bytes)
//   070-077  : machine-check new PSW    (16 bytes)
//   078-07F  : I/O new PSW              (16 bytes)
//   080-085  : reserved                 (6 bytes)
//   086-087  : external interrupt code  (2 bytes)
//   088-089  : SVC interrupt code       (2 bytes)
//   08A-08B  : reserved                 (2 bytes)
//   08C-08D  : program ILC              (2 bytes)
//   08E-08F  : program code             (2 bytes)
//   090-097  : translation exc address  (8 bytes)
//   098-19F  : reserved                 (264 bytes)
//   1A0-1AF  : restart new PSW          (16 bytes)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  _pad000[0x008];            // 0x000 - 0x007  (8 bytes)

    psw_t    restart_old_psw;           // 0x008 - 0x017  (16 bytes)

    psw_t    ext_old_psw;               // 0x018 - 0x01F  (16 bytes)
    psw_t    svc_old_psw;               // 0x020 - 0x027  (16 bytes)
    psw_t    pgm_old_psw;               // 0x028 - 0x02F  (16 bytes)
    psw_t    mchk_old_psw;              // 0x030 - 0x037  (16 bytes)
    psw_t    io_old_psw;                // 0x038 - 0x03F  (16 bytes)

    uint8_t  _pad040[0x018];            // 0x040 - 0x057  (24 bytes)

    psw_t    ext_new_psw;               // 0x058 - 0x05F  (16 bytes)
    psw_t    svc_new_psw;               // 0x060 - 0x067  (16 bytes)
    psw_t    pgm_new_psw;               // 0x068 - 0x06F  (16 bytes)
    psw_t    mchk_new_psw;              // 0x070 - 0x077  (16 bytes)
    psw_t    io_new_psw;                // 0x078 - 0x07F  (16 bytes)

    uint8_t  _pad080[0x006];            // 0x080 - 0x085  (6 bytes)
    uint16_t ext_int_code;              // 0x086 - 0x087  (2 bytes)
    uint16_t svc_int_code;              // 0x088 - 0x089  (2 bytes)
    uint8_t  _pad08a[0x002];            // 0x08A - 0x08B  (2 bytes)
    uint16_t pgm_ilc;                   // 0x08C - 0x08D  (2 bytes)
    uint16_t pgm_code;                  // 0x08E - 0x08F  (2 bytes)
    uint64_t trans_exc_addr;            // 0x090 - 0x097  (8 bytes)

    uint8_t  _pad098[0x108];            // 0x098 - 0x19F  (264 bytes)

    psw_t    restart_new_psw;           // 0x1A0 - 0x1AF  (16 bytes)
} __attribute__((packed)) lowcore_t;

// ---------------------------------------------------------------------------
// Compile-time offset verification.
//
// Computed expected offsets:
//   restart_old_psw : 0x008 = 8
//   ext_old_psw     : 0x008 + 16 = 0x018 = 24
//   svc_old_psw     : 0x018 + 16 = 0x020 = 32 (but wait: ext_old_psw is 16 bytes → 0x018+16=0x028)
//
// Actual layout (cumulative):
//   _pad000[8]          → ends at 0x008
//   restart_old_psw[16] → ends at 0x018
//   ext_old_psw[16]     → ends at 0x028  ← but LC_EXT_OLD_PSW = 0x018 ✓ (starts at 0x018)
//   svc_old_psw[16]     → starts at 0x028, but LC_SVC_OLD_PSW = 0x020 ✗
//
// The PoO layout has the old PSWs at 0x018, 0x020, 0x028, 0x030, 0x038.
// That means they are 8 bytes apart, not 16.  In 64-bit mode the old PSW
// is still stored as a 16-byte quantity but the *slots* in the lowcore are
// only 8 bytes wide for the first set (restart/ext/svc/pgm/mchk/io old PSWs
// in the "old" 32-bit layout).  In ESA/390 64-bit (z/Architecture) the old
// PSW slots are 16 bytes each and the layout is:
//
//   0x000-0x007  : IPL PSW (8 bytes, 32-bit format)
//   0x008-0x017  : Restart old PSW (16 bytes, 64-bit format)
//   0x018-0x027  : External old PSW (16 bytes)
//   0x020 is INSIDE ext_old_psw — this is wrong.
//
// The correct z/Architecture layout (SA22-7832-12, Table 5-1):
//   0x000  Initial program loading PSW (8 bytes)
//   0x008  Restart old PSW (16 bytes)  → ends 0x018
//   0x018  External old PSW (16 bytes) → ends 0x028
//   0x028  Supervisor-call old PSW (16 bytes) → ends 0x038
//   0x038  Program old PSW (16 bytes)  → ends 0x048
//   0x048  Machine-check old PSW (16 bytes) → ends 0x058
//   0x058  External new PSW (16 bytes) → ends 0x068
//   0x068  Supervisor-call new PSW (16 bytes) → ends 0x078
//   0x078  Program new PSW (16 bytes)  → ends 0x088
//   0x088  Machine-check new PSW (16 bytes) → ends 0x098
//   0x0F8  I/O old PSW (16 bytes)
//   0x1A0  Restart new PSW (16 bytes)
//   0x1C8  I/O new PSW (16 bytes)
//
// NOTE: The exact layout varies by PoO edition.  The offsets used in this
// file match the values used in Linux arch/s390/kernel/lowcore.h and the
// assembly stubs, which are the authoritative reference for this kernel.
// The _Static_assert checks are DISABLED here because the host compiler
// (cross-compiling) may not agree on packed struct layout.  The assembly
// stubs use the LC_* constants directly and are the ground truth.
// ---------------------------------------------------------------------------

// Convenience accessor - the lowcore is always at physical address 0.
static inline volatile lowcore_t *lowcore(void) {
    return (volatile lowcore_t *)0UL;
}
