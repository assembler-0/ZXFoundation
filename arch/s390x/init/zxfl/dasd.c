// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/dasd.c
//
/// @brief DASD I/O engine for the ZXFL bootloader.
///        Implements synchronous SSCH/TSCH channel I/O, single-record reads
///        with automatic track/cylinder advancement, and VTOC dataset search.

#include <arch/s390x/init/zxfl/dasd.h>
#include <arch/s390x/init/zxfl/ebcdic.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline void zxfl_memset(void *s, int c, uint32_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
}

static inline int zxfl_memcmp(const void *s1, const void *s2, uint32_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++; p2++;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Synchronous SSCH/TSCH I/O
// ---------------------------------------------------------------------------

/// @brief Issue SSCH and spin on TSCH until the operation completes.
///        The ORB is built here; the caller provides the CCW chain.
///
///        We spin rather than use interrupts because the bootloader runs
///        with interrupts disabled and has no interrupt handler infrastructure.
///        The TSCH loop is safe: CC=1 means "status not yet available, retry".
int dasd_sync_io(uint32_t schid, ccw1_t *ccw) {
    orb_t orb;
    zxfl_memset(&orb, 0, sizeof(orb));
    // Format-1 CCWs (F bit), LPM=0xFF (try all paths), no prefetch.
    orb.flags = ORB_FLAGS_F1_LPM_FF;
    orb.cpa   = (uint32_t)(uintptr_t)ccw;

    // R1 must hold the subchannel ID for SSCH/TSCH.
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

    // IRB is 96 bytes (24 words).  We only inspect SCSW word 2 (irb[2])
    // which contains the device/channel status after completion.
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
        // CC=1: status not yet available — spin.
        // CC=0: status stored in IRB.
        // CC=2: subchannel busy (should not happen after SSCH CC=0).
        // CC=3: not operational.
    } while (cc == 1);

    if (cc != 0) return -1;

    // SCSW word 2 layout (big-endian, s390x is natively big-endian):
    //   bits 0-7  : device status  (0x02=Unit Check, 0x01=Unit Exception)
    //   bits 8-15 : channel status (any non-zero = channel error)
    //   bits 16-31: residual count
    uint32_t status = irb[2];
    if (status & 0x0200U) return -1;   // Unit Check
    if (status & 0x0100U) return -1;   // Unit Exception
    if (status & 0xFF0000U) return -1; // Channel status error

    return 0;
}

// ---------------------------------------------------------------------------
// SENSE — clear device unit-check state
// ---------------------------------------------------------------------------

/// @brief Issue a Basic Sense (0x04) to clear a unit-check condition.
///        A 3390 in unit-check state rejects all subsequent commands with
///        CMDREJ until sense data is collected.  We discard the sense bytes;
///        we only need the side-effect of clearing the condition.
static void dasd_sense(uint32_t schid) {
    static uint8_t  sense_buf[32] __attribute__((aligned(4)));
    static ccw1_t   sense_ccw    __attribute__((aligned(8)));

    sense_ccw.cmd   = 0x04U; // Basic Sense
    sense_ccw.flags = CCW_FLAG_SLI;
    sense_ccw.count = sizeof(sense_buf);
    sense_ccw.cda   = (uint32_t)(uintptr_t)sense_buf;

    // Ignore the return value — if sense itself fails the device is
    // truly not operational and the caller will handle it.
    dasd_sync_io(schid, &sense_ccw);
}

// ---------------------------------------------------------------------------
// Single-record read
// ---------------------------------------------------------------------------

/// @brief Build a 4-CCW seek/search/TIC/read chain and execute it.
///
///        The chain is static to avoid stack allocation in a freestanding
///        environment where stack size is constrained to CONFIG_BOOT_STACK_SIZE.
///        Static storage is safe here because the bootloader is single-threaded.
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
    chain[0].count = sizeof(dasd_seek_arg_t);
    chain[0].cda   = (uint32_t)(uintptr_t)&seek_arg;

    // CCW[1]: SEARCH ID EQ — compare CCHHR against the current record ID.
    chain[1].cmd   = CCW_CMD_SEARCH_ID_EQ;
    chain[1].flags = CCW_FLAG_CC_SLI;
    chain[1].count = sizeof(dasd_search_arg_t);
    chain[1].cda   = (uint32_t)(uintptr_t)&search_arg;

    // CCW[2]: TIC back to CCW[1] — the channel retries SEARCH until it matches.
    //         This is the standard DASD positioning idiom; without it the
    //         channel would give a "record not found" unit check if the disk
    //         rotated past the target record before SEARCH ran.
    chain[2].cmd   = CCW_CMD_TIC;
    chain[2].flags = 0;
    chain[2].count = 0;
    chain[2].cda   = (uint32_t)(uintptr_t)&chain[1];

    // CCW[3]: READ DATA or READ KEY+DATA — transfer the record into buf.
    //         SLI suppresses the length-mismatch unit check when len < record size.
    chain[3].cmd   = rd_cmd;
    chain[3].flags = CCW_FLAG_SLI;
    chain[3].count = (uint16_t)len;
    chain[3].cda   = (uint32_t)(uintptr_t)buf;

    return dasd_sync_io(schid, chain);
}

// ---------------------------------------------------------------------------
// Multi-track sequential read
// ---------------------------------------------------------------------------

/// @brief Advance (cyl, head, rec) to the next record, wrapping across
///        tracks and cylinders.  rec is 1-based.
///
///        On a 3390, each track holds a variable number of records depending
///        on record size.  We use a conservative limit derived from track
///        capacity: floor(DASD_3390_BYTES_PER_TRK / DASD_BLOCK_SIZE).
///        For 4096-byte blocks: 56664 / 4096 = 13 records per track.
///        We use 12 to be safe (leaves room for the R0 home address record).
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

// ---------------------------------------------------------------------------
// VTOC dataset search
// ---------------------------------------------------------------------------

/// @brief Parse a Format-1 DSCB from a raw buffer, handling both
///        44-byte-key (key+data, 140 bytes) and 0-byte-key (data-only, 96 bytes)
///        layouts.
///
///        The key-length ambiguity is resolved by probing: we first attempt
///        a Read Key+Data (0x0E) into a 140-byte buffer.  If the format byte
///        at offset DSCB1_KD_FMTID_OFF (44) is 0xF1, we have a key+data DSCB.
///        If it is not 0xF1 but the byte at DSCB1_D_FMTID_OFF (44) in a
///        data-only interpretation is 0xF1, we treat it as data-only.
///        In practice, for Hercules virtual DASD the key is always present,
///        but we handle the degenerate case for correctness.
///
///        Returns 1 if this DSCB matches dsname_ebcdic, 0 if not, -1 on error.
static int dscb_match(const uint8_t *buf, uint32_t buf_len,
                      const uint8_t *dsname_ebcdic,
                      dscb1_extent_t *out_extent) {
    // Determine layout: key+data (140 bytes) or data-only (96 bytes).
    // The dataset name occupies the first 44 bytes in both layouts.
    // The format ID is at byte 44 in both layouts.
    // The first extent is at byte 108 (key+data) or byte 64 (data-only).

    uint32_t fmtid_off;
    uint32_t extent_off;

    if (buf_len >= 140U && buf[DSCB1_KD_FMTID_OFF] == DSCB_FMT1_ID) {
        // Standard key+data layout.
        fmtid_off  = DSCB1_KD_FMTID_OFF;
        extent_off = DSCB1_KD_EXTENT0_OFF;
    } else if (buf_len >= 96U && buf[DSCB1_D_FMTID_OFF] == DSCB_FMT1_ID) {
        // 0-byte-key layout: data only.
        fmtid_off  = DSCB1_D_FMTID_OFF;
        extent_off = DSCB1_D_EXTENT0_OFF;
    } else {
        // Not a Format-1 DSCB (could be F4 VTOC header, F5 free space, etc.)
        return 0;
    }
    (void)fmtid_off; // used only for layout selection above

    // Compare the 44-byte EBCDIC dataset name.
    if (zxfl_memcmp(buf, dsname_ebcdic, 44U) != 0) return 0;

    // Extract the first extent descriptor (10 bytes at extent_off):
    //   [0]    XTTYPE  — extent type (ignored here)
    //   [1]    XTSEQN  — sequence number (ignored)
    //   [2..3] XTBCYL  — begin cylinder (big-endian)
    //   [4..5] XTBTRK  — begin head (big-endian)
    //   [6..7] XTECYL  — end cylinder (big-endian)
    //   [8..9] XTETRK  — end head (big-endian)
    const uint8_t *ext = buf + extent_off;
    out_extent->begin_cyl  = (uint16_t)((ext[2] << 8) | ext[3]);
    out_extent->begin_head = (uint16_t)((ext[4] << 8) | ext[5]);
    out_extent->end_cyl    = (uint16_t)((ext[6] << 8) | ext[7]);
    out_extent->end_head   = (uint16_t)((ext[8] << 8) | ext[9]);

    return 1;
}

int dasd_find_dataset(uint32_t schid,
                      const char *dsname,
                      dscb1_extent_t *out_extent) {
    // Build the 44-byte EBCDIC space-padded dataset name for comparison.
    uint8_t target[44];
    zxfl_memset(target, 0x40U, 44U); // 0x40 = EBCDIC space
    for (int i = 0; dsname[i] != '\0' && i < 44; i++) {
        target[i] = ascii_to_ebcdic((uint8_t)dsname[i]);
    }

    // Read-Key+Data buffer: 44-byte key + 96-byte data = 140 bytes.
    // Aligned to 4 bytes to satisfy channel data address requirements.
    static uint8_t dscb_buf[140] __attribute__((aligned(4)));

    // Maximum retries per record before giving up and moving on.
    // This prevents the CMDREJ infinite loop: a unit check leaves the
    // device in a state where it rejects all commands until SENSE is
    // issued.  We issue SENSE and advance to the next record rather than
    // retrying the same one forever.
#define DASD_MAX_RETRIES_PER_REC  3U

    for (uint8_t rec = 1; rec <= VTOC_MAX_RECORDS; rec++) {
        int rc = -1;
        uint32_t buf_len = 0;

        for (uint32_t attempt = 0; attempt < DASD_MAX_RETRIES_PER_REC; attempt++) {
            // Attempt Read Key+Data first (standard 44-byte key layout).
            rc = dasd_read_record(schid,
                                  VTOC_DEFAULT_CYL, VTOC_DEFAULT_HEAD,
                                  rec, CCW_CMD_READ_KD,
                                  dscb_buf, 140U);
            if (rc == 0) { buf_len = 140U; break; }

            // Unit check — issue SENSE to clear device state, then retry
            // with Read Data only (handles 0-byte-key virtual disks).
            dasd_sense(schid);

            rc = dasd_read_record(schid,
                                  VTOC_DEFAULT_CYL, VTOC_DEFAULT_HEAD,
                                  rec, CCW_CMD_READ_DATA,
                                  dscb_buf, 96U);
            if (rc == 0) { buf_len = 96U; break; }

            // Both commands failed — clear state and try again.
            dasd_sense(schid);
        }

        if (rc < 0) {
            // Record is unreadable after all retries.  On a real VTOC,
            // unreadable records past the last DSCB mean end-of-VTOC.
            // We stop here rather than spinning forever.
            break;
        }

        int match = dscb_match(dscb_buf, buf_len, target, out_extent);
        if (match == 1) return 0; // Found.
    }

    return -1; // Not found.
}
