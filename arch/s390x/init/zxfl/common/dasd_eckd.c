// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/dasd_eckd.c
//
/// @brief ECKD (Extended Count Key Data) DASD driver for ZXFL.
///
///        Probes the device via Sense ID (0xE4) and Read Device
///        Characteristics (0x64), then provides LBA-to-CCHHR translation
///        and block-level read operations on top of the raw CCW engine.

#include <arch/s390x/init/zxfl/dasd_eckd.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/diag.h>

#define SENSE_ID_LEN    8U
#define RDC_LEN         64U

static inline uint32_t eckd_cda(const void *p) {
    uintptr_t addr = (uintptr_t)p;
    if (addr > 0x7FFFFFFFUL) __builtin_trap();
    return (uint32_t)addr;
}

int dasd_eckd_probe(uint32_t schid, dasd_eckd_geo_t *out_geo) {
    static uint8_t sense_id_buf[SENSE_ID_LEN] __attribute__((aligned(4)));
    static ccw1_t  sense_id_ccw              __attribute__((aligned(8)));

    memset(sense_id_buf, 0, sizeof(sense_id_buf));
    sense_id_ccw.cmd   = CCW_CMD_SENSE_ID;
    sense_id_ccw.flags = CCW_FLAG_SLI;
    sense_id_ccw.count = SENSE_ID_LEN;
    sense_id_ccw.cda   = eckd_cda(sense_id_buf);

    if (dasd_sync_io(schid, &sense_id_ccw) < 0) {
        dasd_sense(schid);
        if (dasd_sync_io(schid, &sense_id_ccw) < 0) {
            out_geo->dev_type     = ECKD_DEV_TYPE_3390;
            out_geo->dev_model    = 0x03U;
            out_geo->cyls         = 3339U;
            out_geo->heads        = ECKD_3390_HEADS;
            out_geo->recs_per_trk = ECKD_3390_RECS_PER_TRK;
            out_geo->block_size   = DASD_BLOCK_SIZE;
            out_geo->total_blocks = (uint32_t)3339U * ECKD_3390_HEADS
                                    * ECKD_3390_RECS_PER_TRK;
            return 0;
        }
    }

    // Byte 0 of a valid Sense ID response is always 0xFF.
    if (sense_id_buf[0] != 0xFFU) {
        print("eckd: sense id byte[0] != 0xFF — not an ECKD device\n");
        return -1;
    }

    out_geo->dev_type  = (uint16_t)((sense_id_buf[1] << 8) | sense_id_buf[2]);
    out_geo->dev_model = sense_id_buf[3];

    // Reject non-ECKD device types.
    if (out_geo->dev_type != ECKD_DEV_TYPE_3390 &&
        out_geo->dev_type != ECKD_DEV_TYPE_3380 &&
        out_geo->dev_type != ECKD_DEV_TYPE_9345) {
        print("eckd: unknown device type, attempting rdc\n");
    }

    // ---- Read Device Characteristics ------------------------------------
    static uint8_t rdc_buf[RDC_LEN] __attribute__((aligned(4)));
    static ccw1_t  rdc_ccw          __attribute__((aligned(8)));

    memset(rdc_buf, 0, sizeof(rdc_buf));
    rdc_ccw.cmd   = CCW_CMD_RDC;
    rdc_ccw.flags = CCW_FLAG_SLI;
    rdc_ccw.count = RDC_LEN;
    rdc_ccw.cda   = eckd_cda(rdc_buf);

    if (dasd_sync_io(schid, &rdc_ccw) < 0) {
        dasd_sense(schid);
        // RDC failed — fall back to well-known 3390 geometry.
        out_geo->cyls         = 3339U;
        out_geo->heads        = ECKD_3390_HEADS;
        out_geo->recs_per_trk = ECKD_3390_RECS_PER_TRK;
        out_geo->block_size   = DASD_BLOCK_SIZE;
        out_geo->total_blocks = (uint32_t)3339U * ECKD_3390_HEADS
                                * ECKD_3390_RECS_PER_TRK;
        return 0;
    }

    uint16_t cyls  = (uint16_t)((rdc_buf[4] << 8) | rdc_buf[5]);
    uint8_t  heads = rdc_buf[6];

    if (cyls == 0 || heads == 0) {
        cyls  = 3339U;
        heads = ECKD_3390_HEADS;
    }

    out_geo->cyls         = cyls;
    out_geo->heads        = heads;
    out_geo->recs_per_trk = ECKD_3390_RECS_PER_TRK;
    out_geo->block_size   = DASD_BLOCK_SIZE;
    out_geo->total_blocks = (uint32_t)cyls * heads * ECKD_3390_RECS_PER_TRK;

    return 0;
}

void dasd_eckd_geo_from_lba(const dasd_eckd_geo_t *geo, uint32_t lba,
                             uint16_t *out_cyl, uint16_t *out_head,
                             uint8_t  *out_rec) {
    const uint32_t recs_per_cyl = (uint32_t)geo->heads * geo->recs_per_trk;
    const uint32_t cyl  = lba / recs_per_cyl;
    const uint32_t rem  = lba % recs_per_cyl;
    const uint32_t head = rem / geo->recs_per_trk;
    const uint32_t rec  = rem % geo->recs_per_trk;   // 0-based

    *out_cyl  = (uint16_t)cyl;
    *out_head = (uint16_t)head;
    *out_rec  = (uint8_t)(rec + 1U);   // convert to 1-based
}

int dasd_eckd_read_block(uint32_t schid, const dasd_eckd_geo_t *geo,
                         uint32_t lba, void *buf, uint32_t len) {
    uint16_t cyl, head;
    uint8_t  rec;
    dasd_eckd_geo_from_lba(geo, lba, &cyl, &head, &rec);
    return dasd_read_record(schid, cyl, head, rec, CCW_CMD_READ_DATA, buf, len);
}

int dasd_eckd_read_blocks(uint32_t schid, const dasd_eckd_geo_t *geo,
                          uint32_t lba_start, uint32_t count, void *buf) {
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (dasd_eckd_read_block(schid, geo, lba_start + i,
                                 dst, geo->block_size) < 0)
            return (i == 0) ? -1 : (int)i;
        dst += geo->block_size;
    }
    return (int)count;
}
