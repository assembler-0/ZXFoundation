// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/dasd_io.c
//
/// @brief Raw DASD channel I/O engine for the ZXFL bootloader.
///
///        Implements synchronous SSCH/TSCH channel I/O, SENSE, single-record
///        reads with a 4-CCW SEEK/SEARCH/TIC/READ chain, and sequential
///        record advancement across track and cylinder boundaries.
///
///        All functions are single-threaded and use static storage for CCW
///        chains and I/O arguments to avoid stack pressure in a freestanding
///        environment where CONFIG_BOOT_STACK_SIZE is limited.

#include <arch/s390x/init/zxfl/dasd_io.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline void zxfl_memset_io(void *s, int c, uint32_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
}

// ---------------------------------------------------------------------------
// Synchronous SSCH/TSCH I/O
// ---------------------------------------------------------------------------

int dasd_sync_io(uint32_t schid, ccw1_t *ccw) {
    orb_t orb;
    zxfl_memset_io(&orb, 0, sizeof(orb));
    // Format-1 CCWs (F bit set in flags), LPM=0xFF (try all paths).
    orb.flags = ORB_FLAGS_F1_LPM_FF;
    orb.cpa   = (uint32_t)(uintptr_t)ccw;

    // R1 must hold the subchannel ID for SSCH and TSCH.
    register uint32_t r1 __asm__("1") = schid;
    int cc;

    __asm__ volatile (
        "ssch %1\n"
        "ipm  %0\n"
        "srl  %0, 28\n"
        : "=d" (cc)
        : "m" (orb), "d" (r1)
        : "cc", "memory"
    );
    if (cc != 0) return -1;

    // IRB is 96 bytes (24 words).  We inspect SCSW word 2 (irb[2]) which
    // contains device/channel status after the operation completes.
    //
    // TSCH condition codes:
    //   CC=0: status stored in IRB — operation complete.
    //   CC=1: status not yet available — spin and retry.
    //   CC=2: subchannel busy (should not occur after SSCH CC=0).
    //   CC=3: not operational.
    uint32_t irb[24];
    do {
        __asm__ volatile (
            "tsch %1\n"
            "ipm  %0\n"
            "srl  %0, 28\n"
            : "=d" (cc)
            : "m" (irb), "d" (r1)
            : "cc", "memory"
        );
    } while (cc == 1);

    if (cc != 0) return -1;

    // IRB layout (96 bytes = 24 uint32_t words, all big-endian):
    //   irb[0..2]  = SCSW (Subchannel Status Word, 12 bytes)
    //   irb[3..7]  = ESWS (Extended Status Word Set)
    //   irb[8..23] = EMWS (Extended Measurement Word Set)
    //
    // SCSW byte layout (12 bytes):
    //   Bytes 0-3  (irb[0]): Flags (key, S, L, CC, F, P, I, A, U, Z, E, N, Q)
    //   Bytes 4-7  (irb[1]): CCW address
    //   Bytes 8-11 (irb[2]): Device Status (byte 8) | Channel Status (byte 9)
    //                        | Residual Count (bytes 10-11)
    //
    // s390x is big-endian.  irb[2] as a uint32_t:
    //   Bits 31-24 (MSB byte): Device Status
    //     Bit 25 (0x04000000) = Attention
    //     Bit 24 (0x02000000) = Status Modifier
    //     Bit 23 (0x01000000) = Control Unit End
    //     Bit 22 (0x00800000) = Busy
    //     Bit 21 (0x00400000) = Channel End
    //     Bit 20 (0x00200000) = Device End
    //     Bit 19 (0x00100000) = Unit Check    ← 0x02 in the byte = 0x02000000 in uint32
    //     Bit 18 (0x00080000) = Unit Exception ← 0x01 in the byte = 0x01000000 in uint32
    //   Bits 23-16 (next byte): Channel Status
    //     Any non-zero = channel error
    //   Bits 15-0: Residual count (ignored when SLI is set)
    //
    // The previous code used 0x0200 and 0x0100 which are bits in the
    // RESIDUAL COUNT field, not the status bytes.  Correct masks:
    uint32_t status = irb[2];
    if (status & 0x02000000U) return -1;   // Unit Check    (device status bit)
    if (status & 0x01000000U) return -1;   // Unit Exception (device status bit)
    if (status & 0x00FF0000U) return -1;   // Channel Status (any bit = error)

    return 0;
}

// ---------------------------------------------------------------------------
// SENSE — clear device unit-check state
// ---------------------------------------------------------------------------

void dasd_sense(uint32_t schid) {
    // Static to avoid stack pressure; single-threaded bootloader.
    static uint8_t  sense_buf[32] __attribute__((aligned(4)));
    static ccw1_t   sense_ccw     __attribute__((aligned(8)));

    sense_ccw.cmd   = CCW_CMD_SENSE;
    sense_ccw.flags = CCW_FLAG_SLI;
    sense_ccw.count = (uint16_t)sizeof(sense_buf);
    sense_ccw.cda   = (uint32_t)(uintptr_t)sense_buf;

    // Ignore the return value — if SENSE itself fails the device is
    // truly not operational and the caller will handle it.
    dasd_sync_io(schid, &sense_ccw);
}

// ---------------------------------------------------------------------------
// Single-record read
// ---------------------------------------------------------------------------

int dasd_read_record(uint32_t schid,
                     uint16_t cyl, uint16_t head, uint8_t rec,
                     uint8_t rd_cmd, void *buf, uint32_t len) {
    // Static to avoid stack pressure; single-threaded bootloader.
    static dasd_seek_arg_t   seek_arg;
    static dasd_search_arg_t search_arg;
    static ccw1_t            chain[4] __attribute__((aligned(8)));

    seek_arg.reserved = 0;
    seek_arg.cyl      = cyl;
    seek_arg.head     = head;

    search_arg.cyl  = cyl;
    search_arg.head = head;
    search_arg.rec  = rec;

    // CCW[0]: SEEK — position the heads to the target cylinder/head.
    chain[0].cmd   = CCW_CMD_SEEK;
    chain[0].flags = CCW_FLAG_CC_SLI;
    chain[0].count = (uint16_t)sizeof(dasd_seek_arg_t);
    chain[0].cda   = (uint32_t)(uintptr_t)&seek_arg;

    // CCW[1]: SEARCH ID EQ — compare CCHHR against the current record ID.
    chain[1].cmd   = CCW_CMD_SEARCH_ID_EQ;
    chain[1].flags = CCW_FLAG_CC_SLI;
    chain[1].count = (uint16_t)sizeof(dasd_search_arg_t);
    chain[1].cda   = (uint32_t)(uintptr_t)&search_arg;

    // CCW[2]: TIC back to CCW[1] — the channel retries SEARCH until it
    //         matches.  Without this, the channel gives a "record not found"
    //         unit check if the disk rotated past the target record before
    //         SEARCH ran.
    chain[2].cmd   = CCW_CMD_TIC;
    chain[2].flags = 0;
    chain[2].count = 0;
    chain[2].cda   = (uint32_t)(uintptr_t)&chain[1];

    // CCW[3]: READ DATA or READ KEY+DATA — transfer the record into buf.
    //         SLI suppresses the length-mismatch unit check when len is
    //         smaller than the actual record size (e.g. reading 96 bytes
    //         from a 140-byte DSCB).
    chain[3].cmd   = rd_cmd;
    chain[3].flags = CCW_FLAG_SLI;
    chain[3].count = (uint16_t)len;
    chain[3].cda   = (uint32_t)(uintptr_t)buf;

    return dasd_sync_io(schid, chain);
}

// ---------------------------------------------------------------------------
// Multi-track sequential read
// ---------------------------------------------------------------------------

/// Records per track for 4096-byte fixed-block records on a 3390.
/// floor(56664 / 4096) = 13; we use 12 to leave room for the R0 home-address
/// record and avoid off-by-one errors at track boundaries.
#define ZXFL_RECS_PER_TRACK     12U

int dasd_read_next(uint32_t schid,
                   uint16_t *cyl, uint16_t *head, uint8_t *rec,
                   uint8_t rd_cmd, void *buf, uint32_t len) {
    int rc = dasd_read_record(schid, *cyl, *head, *rec, rd_cmd, buf, len);
    if (rc < 0) return rc;

    // Advance position for the next call.
    (*rec)++;
    if (*rec > ZXFL_RECS_PER_TRACK) {
        *rec = 1;
        (*head)++;
        if (*head >= DASD_3390_HEADS_PER_CYL) {
            *head = 0;
            (*cyl)++;
        }
    }
    return 0;
}
