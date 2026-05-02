// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/lowcore.h
//
/// @brief z/Architecture lowcore layout and loader setup API.
///
///        The z/Architecture lowcore occupies physical addresses 0x000–0x1FF
///        (the first 512 bytes of memory).  It is a hardware-defined structure:
///        the CPU reads and writes specific offsets for PSW saves, new PSWs,
///        prefix register, etc.
///
///        The loader must install valid "new PSW" entries for at least the
///        restart, external, I/O, machine-check, and program-check slots
///        before handing off to the kernel.  Without these, any interrupt
///        (including a spurious machine check from Hercules) will cause the
///        CPU to load a zero PSW and halt with an addressing exception.
///
///        We install disabled-wait PSWs pointing to distinct halt addresses
///        so that if an unexpected interrupt fires before the kernel installs
///        its own handlers, the operator can identify which interrupt type
///        caused the halt from the PSW address.

#ifndef ZXFOUNDATION_ZXFL_LOWCORE_H
#define ZXFOUNDATION_ZXFL_LOWCORE_H

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// z/Architecture PSW (16 bytes, big-endian)
// ---------------------------------------------------------------------------

/// @brief 64-bit z/Architecture PSW.
///        Stored as two 64-bit words: psw_mask and psw_addr.
typedef struct __attribute__((packed, aligned(8))) {
    uint64_t mask;  ///< PSW mask word (bits 0-63)
    uint64_t addr;  ///< PSW address word (bits 64-127)
} zxfl_psw_t;

/// @brief PSW mask: 64-bit mode, DAT off, all interrupts masked, wait state set.
///        Bit 16 (wait) = 1, bits 31+32 (64-bit addressing) = 1.
///        All interrupt mask bits (13=I/O, 14=ext, 15=mck) = 0.
#define ZXFL_PSW_MASK_64BIT_DISABLED    UINT64_C(0x0000800180000000)

// ---------------------------------------------------------------------------
// Lowcore layout (physical 0x000–0x1FF)
// Offsets are per z/Architecture Principles of Operation, SA22-7832.
// Only the fields the loader touches are named; the rest are uint8_t arrays.
// ---------------------------------------------------------------------------

/// @brief Overlay of the z/Architecture lowcore.
///        Cast a pointer to physical address 0x0 to this type.
///        NEVER allocate this struct — it IS the hardware lowcore.
///
///        Offsets per z/Architecture PoP SA22-7832.
///        In z/Architecture, all PSWs are 16 bytes (64-bit format).
///        The restart new PSW lives at 0x1A0, inside _tail.
typedef struct __attribute__((packed)) {
    // 0x000 – 0x00F: IPL PSW (written by channel subsystem at IPL)
    zxfl_psw_t      ipl_psw;                    // 0x000

    // 0x010 – 0x01F: External old PSW
    zxfl_psw_t      ext_old_psw;                // 0x010

    // 0x020 – 0x02F: SVC old PSW
    zxfl_psw_t      svc_old_psw;                // 0x020

    // 0x030 – 0x03F: Program-check old PSW
    zxfl_psw_t      pgm_old_psw;                // 0x030

    // 0x040 – 0x04F: Machine-check old PSW
    zxfl_psw_t      mck_old_psw;                // 0x040

    // 0x050 – 0x05F: I/O old PSW
    zxfl_psw_t      io_old_psw;                 // 0x050

    // 0x060 – 0x07F: Reserved (two 16-byte slots; 0x070 is NOT restart new PSW
    //                in z/Architecture — restart new PSW is at 0x1A0)
    uint8_t         _rsv0[32];                  // 0x060

    // 0x080 – 0x08F: External new PSW
    zxfl_psw_t      ext_new_psw;                // 0x080

    // 0x090 – 0x09F: SVC new PSW
    zxfl_psw_t      svc_new_psw;                // 0x090

    // 0x0A0 – 0x0AF: Program-check new PSW
    zxfl_psw_t      pgm_new_psw;                // 0x0A0

    // 0x0B0 – 0x0BF: Machine-check new PSW
    zxfl_psw_t      mck_new_psw;                // 0x0B0

    // 0x0C0 – 0x0CF: I/O new PSW
    zxfl_psw_t      io_new_psw;                 // 0x0C0

    // 0x0D0 – 0x0FF: Reserved / hardware use
    uint8_t         _rsv1[48];                  // 0x0D0

    // 0x100 – 0x107: External interrupt code + CPU address
    uint16_t        ext_int_code;               // 0x100
    uint16_t        ext_cpu_addr;               // 0x102
    uint32_t        _rsv2;                      // 0x104

    // 0x108 – 0x10F: SVC interrupt code
    uint16_t        svc_int_code;               // 0x108
    uint8_t         _rsv3[6];                   // 0x10A

    // 0x110 – 0x117: Program interrupt code
    uint32_t        pgm_int_code;               // 0x110
    uint8_t         _rsv4[4];                   // 0x114

    // 0x118 – 0x11F: Translation exception address
    uint64_t        trans_exc_addr;             // 0x118

    // 0x120 – 0x127: Monitor class / machine-check interrupt code
    uint8_t         _rsv5[8];                   // 0x120

    // 0x128 – 0x12F: I/O interrupt subsystem ID word
    uint32_t        io_int_ssid;                // 0x128
    uint32_t        io_int_parm;                // 0x12C

    // 0x130 – 0x137: I/O interrupt identification word
    uint32_t        io_int_id;                  // 0x130
    uint32_t        _rsv6;                      // 0x134

    // 0x138 – 0x13F: Subchannel ID at IPL (set by channel subsystem)
    uint32_t        ipl_schid;                  // 0x138 (note: also at 0xB8 in 31-bit)
    uint32_t        _rsv7;                      // 0x13C

    // 0x140 – 0x1FF: Remaining lowcore (CPU timer, clock comparator, etc.)
    uint8_t         _tail[192];                 // 0x140
} zxfl_lowcore_t;

// Compile-time size check: lowcore must be exactly 512 bytes.
_Static_assert(sizeof(zxfl_lowcore_t) == 512,
               "zxfl_lowcore_t must be exactly 512 bytes");

/// @brief Physical address of the lowcore.
#define ZXFL_LOWCORE_PHYS       UINT64_C(0x0)

/// @brief Halt addresses for unexpected interrupts.
///        All point to 0x0 (always mapped — the lowcore).
///        The interrupt type is identified by which new PSW was loaded,
///        visible in the Hercules PSW display as the active PSW mask.
#define ZXFL_HALT_EXT           UINT64_C(0x0000000000000000)
#define ZXFL_HALT_SVC           UINT64_C(0x0000000000000000)
#define ZXFL_HALT_PGM           UINT64_C(0x0000000000000000)
#define ZXFL_HALT_MCK           UINT64_C(0x0000000000000000)
#define ZXFL_HALT_IO            UINT64_C(0x0000000000000000)

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// @brief Install safe new PSWs into the lowcore.
///
///        Writes disabled-wait new PSWs for external, SVC, program-check,
///        machine-check, and I/O interrupt slots.  Each PSW points to a
///        distinct halt address so unexpected interrupts are diagnosable.
///
///        Must be called before enabling any interrupts or handing off to
///        the kernel.  The kernel will overwrite these with its own handlers.
void zxfl_lowcore_setup(void);

/// @brief Return a pointer to the lowcore overlay.
///        This is simply a cast of physical address 0x0.
static inline zxfl_lowcore_t *zxfl_lowcore(void) {
    return (zxfl_lowcore_t *)ZXFL_LOWCORE_PHYS;
}

#endif /* ZXFOUNDATION_ZXFL_LOWCORE_H */
