// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/dasd_io.c - Raw DASD channel I/O engine for ZXFL.

#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/string.h>

int dasd_sync_io(uint32_t schid, ccw1_t *ccw) {
    orb_t orb;
    zxfl_memset(&orb, 0, sizeof(orb));
    orb.flags = ORB_FLAGS_F1_LPM_FF;
    orb.cpa   = (uint32_t)(uintptr_t)ccw;

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

    if (cc != 0) return -1;

    uint32_t status = irb[2];
    if (status & 0x02000000U) return -1;   // Unit Check    (device status bit)
    if (status & 0x01000000U) return -1;   // Unit Exception (device status bit)
    if (status & 0x00FF0000U) return -1;   // Channel Status (any bit = error)

    return 0;
}

void dasd_sense(uint32_t schid) {
    static uint8_t  sense_buf[32] __attribute__((aligned(4)));
    static ccw1_t   sense_ccw     __attribute__((aligned(8)));

    sense_ccw.cmd   = CCW_CMD_SENSE;
    sense_ccw.flags = CCW_FLAG_SLI;
    sense_ccw.count = (uint16_t)sizeof(sense_buf);
    sense_ccw.cda   = (uint32_t)(uintptr_t)sense_buf;

    dasd_sync_io(schid, &sense_ccw);
}

int dasd_read_record(uint32_t schid,
                     uint16_t cyl, uint16_t head, uint8_t rec,
                     uint8_t rd_cmd, void *buf, uint32_t len) {
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
    //         matches. 
    chain[2].cmd   = CCW_CMD_TIC;
    chain[2].flags = 0;
    chain[2].count = 0;
    chain[2].cda   = (uint32_t)(uintptr_t)&chain[1];

    // CCW[3]: READ DATA or READ KEY+DATA — transfer the record into buf.
    chain[3].cmd   = rd_cmd;
    chain[3].flags = CCW_FLAG_SLI;
    chain[3].count = (uint16_t)len;
    chain[3].cda   = (uint32_t)(uintptr_t)buf;

    return dasd_sync_io(schid, chain);
}

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
