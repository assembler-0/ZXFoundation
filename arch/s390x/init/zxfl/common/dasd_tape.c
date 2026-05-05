// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/dasd_tape.c
//
/// @brief CCW-based sequential tape driver for ZXFL.
///
///        3480/3490/3590 share the same CCW command set.  The key difference
///        is max block size: 3480/3490 cap at 65535 bytes; 3590 supports up
///        to 256 KB.  We cap at 32768 bytes (TAPE_MAX_BLOCK_DEFAULT) which
///        is safe for all three and fits in a single CCW count field.
///
///        Tape mark detection: when READ FORWARD hits a tape mark, the
///        channel subsystem posts IRB device status with Unit Exception (UE)
///        set and Channel End + Device End set, but NO Channel Check or
///        Device Check.  We inspect the IRB status word directly after TSCH
///        to distinguish EOF from a real error.
///
///        Why we spin on TSCH instead of using interrupts: the bootloader
///        runs with all maskable interrupts disabled (SSM 0x00 in entry.S).
///        Enabling I/O interrupts here would require installing an I/O new
///        PSW and a handler, which is more complexity than the tape path
///        warrants.  Tape rewind on a 3590 takes ~10 seconds worst-case;
///        the spin is acceptable in a bootloader context.

#include <arch/s390x/init/zxfl/dasd_tape.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/diag.h>

#define TAPE_SENSE_ID_LEN       8U
#define TAPE_MAX_BLOCK_DEFAULT  32768U

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline uint32_t tape_cda(const void *p) {
    uintptr_t addr = (uintptr_t)p;
    if (addr > 0x7FFFFFFFUL) __builtin_trap();
    return (uint32_t)addr;
}

/// @brief Execute a single-CCW tape command synchronously.
///        Returns the raw IRB status word (irb[2]) so callers can inspect
///        Unit Exception without a second TSCH.
static int tape_exec_ccw(uint32_t schid, uint8_t cmd, uint8_t flags,
                         void *buf, uint16_t len, uint32_t *out_status) {
    static ccw1_t ccw __attribute__((aligned(8)));
    ccw.cmd   = cmd;
    ccw.flags = flags;
    ccw.count = len;
    ccw.cda   = (buf != nullptr) ? tape_cda(buf) : 0U;

    orb_t orb;
    memset(&orb, 0, sizeof(orb));
    orb.flags = ORB_FLAGS_F1_LPM_FF;
    orb.cpa   = tape_cda(&ccw);

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

    if (out_status) *out_status = irb[2];

    const uint32_t status = irb[2];
    const bool channel_check = (status & 0x00020000U) != 0;  // bit 14
    const bool device_check  = (status & 0x00400000U) != 0;  // bit 17 (corrected: device status byte)

    const uint8_t dev_status = (uint8_t)(status >> 16);
    const bool unit_check     = (dev_status & 0x02U) != 0;   // bit 22 (UnitCheck)
    const bool unit_exception = (dev_status & 0x01U) != 0;   // bit 23 (UnitException)
    (void)channel_check;
    (void)device_check;

    if (unit_check) return -1;
    if (unit_exception) return TAPE_EOF;   // tape mark
    return 0;
}

int dasd_tape_probe(uint32_t schid, dasd_tape_geo_t *out_geo) {
    static uint8_t sense_id_buf[TAPE_SENSE_ID_LEN] __attribute__((aligned(4)));
    memset(sense_id_buf, 0, sizeof(sense_id_buf));

    uint32_t status;
    int rc = tape_exec_ccw(schid, TAPE_CCW_SENSE_ID, CCW_FLAG_SLI,
                           sense_id_buf, TAPE_SENSE_ID_LEN, &status);
    if (rc < 0) {
        // Issue SENSE to clear unit-check, then retry once.
        static uint8_t sense_buf[32] __attribute__((aligned(4)));
        tape_exec_ccw(schid, TAPE_CCW_SENSE, CCW_FLAG_SLI,
                      sense_buf, (uint16_t)sizeof(sense_buf), nullptr);
        rc = tape_exec_ccw(schid, TAPE_CCW_SENSE_ID, CCW_FLAG_SLI,
                           sense_id_buf, TAPE_SENSE_ID_LEN, &status);
        if (rc < 0) {
            print("tape: sense id failed\n");
            return -1;
        }
    }

    if (sense_id_buf[0] != 0xFFU) {
        print("tape: sense id byte[0] != 0xFF\n");
        return -1;
    }

    const uint16_t dev_type = (uint16_t)((sense_id_buf[1] << 8) | sense_id_buf[2]);
    if (dev_type != TAPE_DEV_TYPE_3480 &&
        dev_type != TAPE_DEV_TYPE_3490 &&
        dev_type != TAPE_DEV_TYPE_3590) {
        print("tape: unsupported device type\n");
        return -1;
    }

    out_geo->dev_type  = dev_type;
    out_geo->dev_model = sense_id_buf[3];
    out_geo->max_block = TAPE_MAX_BLOCK_DEFAULT;
    return 0;
}

int dasd_tape_rewind(uint32_t schid) {
    uint32_t status;
    int rc = tape_exec_ccw(schid, TAPE_CCW_REWIND, CCW_FLAG_SLI,
                           nullptr, 0, &status);
    if (rc < 0) {
        static uint8_t sense_buf[32] __attribute__((aligned(4)));
        tape_exec_ccw(schid, TAPE_CCW_SENSE, CCW_FLAG_SLI,
                      sense_buf, (uint16_t)sizeof(sense_buf), nullptr);
        rc = tape_exec_ccw(schid, TAPE_CCW_REWIND, CCW_FLAG_SLI,
                           nullptr, 0, &status);
    }
    return rc;
}

int dasd_tape_read_block(uint32_t schid, void *buf, uint32_t len,
                         uint32_t *out_read) {
    if (len > 0xFFFFU) len = 0xFFFFU;

    uint32_t status;
    int rc = tape_exec_ccw(schid, TAPE_CCW_READ_FWD, CCW_FLAG_SLI,
                           buf, (uint16_t)len, &status);

    if (rc == TAPE_EOF) {
        if (out_read) *out_read = 0;
        return TAPE_EOF;
    }
    if (rc < 0) {
        static uint8_t sense_buf[32] __attribute__((aligned(4)));
        tape_exec_ccw(schid, TAPE_CCW_SENSE, CCW_FLAG_SLI,
                      sense_buf, (uint16_t)sizeof(sense_buf), nullptr);
        return -1;
    }

    if (out_read) *out_read = len;
    return 0;
}

int dasd_tape_fsf(uint32_t schid) {
    uint32_t status;
    int rc = tape_exec_ccw(schid, TAPE_CCW_FSF, CCW_FLAG_SLI,
                           nullptr, 0, &status);
    if (rc == TAPE_EOF) return 0;
    return rc;
}

int dasd_tape_read_file(uint32_t schid, void *buf, uint32_t max_bytes,
                        uint32_t *out_size) {
    uint8_t *dst   = (uint8_t *)buf;
    uint32_t total = 0;

    // Use a static intermediate block to avoid requiring the caller's buffer
    // to be 4-byte aligned at every sub-block boundary.
    static uint8_t block_buf[TAPE_MAX_BLOCK_DEFAULT] __attribute__((aligned(4)));

    while (total < max_bytes) {
        uint32_t avail = max_bytes - total;
        uint32_t xfer  = (avail < TAPE_MAX_BLOCK_DEFAULT)
                         ? avail : TAPE_MAX_BLOCK_DEFAULT;
        uint32_t got   = 0;

        int rc = dasd_tape_read_block(schid, block_buf, xfer, &got);
        if (rc == TAPE_EOF) {
            // Tape mark — end of this file.
            if (out_size) *out_size = total;
            return 0;
        }
        if (rc < 0) return -1;

        if (got > avail) got = avail;
        memcpy(dst + total, block_buf, got);
        total += got;
    }

    if (out_size) *out_size = total;
    return 0;
}
