// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/dasd_fba.c
//
/// @brief FBA (Fixed Block Architecture) DASD driver for ZXFL.
#include <arch/s390x/init/zxfl/dasd_fba.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/diag.h>

static inline uint32_t fba_cda(const void *p) {
    uintptr_t addr = (uintptr_t)p;
    if (addr > 0x7FFFFFFFUL) __builtin_trap();
    return (uint32_t)addr;
}

#define FBA_RDC_LEN     64U
#define FBA_SENSE_ID_LEN 8U

int dasd_fba_probe(uint32_t schid, dasd_fba_geo_t *out_geo) {
    static uint8_t sense_id_buf[FBA_SENSE_ID_LEN] __attribute__((aligned(4)));
    static ccw1_t  sense_id_ccw                   __attribute__((aligned(8)));

    memset(sense_id_buf, 0, sizeof(sense_id_buf));
    sense_id_ccw.cmd   = FBA_CCW_SENSE_ID;
    sense_id_ccw.flags = CCW_FLAG_SLI;
    sense_id_ccw.count = FBA_SENSE_ID_LEN;
    sense_id_ccw.cda   = fba_cda(sense_id_buf);

    if (dasd_sync_io(schid, &sense_id_ccw) < 0) {
        dasd_sense(schid);
        if (dasd_sync_io(schid, &sense_id_ccw) < 0) {
            print("fba: sense id failed\n");
            return -1;
        }
    }

    if (sense_id_buf[0] != 0xFFU) {
        print("fba: sense id byte[0] != 0xFF\n");
        return -1;
    }

    const uint16_t dev_type = (uint16_t)((sense_id_buf[1] << 8) | sense_id_buf[2]);
    if (dev_type != FBA_DEV_TYPE_9336 && dev_type != FBA_DEV_TYPE_0671) {
        print("fba: not an FBA device\n");
        return -1;
    }

    out_geo->dev_type  = dev_type;
    out_geo->dev_model = sense_id_buf[3];

    static uint8_t rdc_buf[FBA_RDC_LEN] __attribute__((aligned(4)));
    static ccw1_t  rdc_ccw              __attribute__((aligned(8)));

    memset(rdc_buf, 0, sizeof(rdc_buf));
    rdc_ccw.cmd   = FBA_CCW_RDC;
    rdc_ccw.flags = CCW_FLAG_SLI;
    rdc_ccw.count = FBA_RDC_LEN;
    rdc_ccw.cda   = fba_cda(rdc_buf);

    if (dasd_sync_io(schid, &rdc_ccw) < 0) {
        dasd_sense(schid);
        out_geo->block_size   = 512U;
        out_geo->total_blocks = 2097152U;   // 1 GB / 512
        return 0;
    }

    uint32_t blk_size = ((uint32_t)rdc_buf[4] << 24) |
                        ((uint32_t)rdc_buf[5] << 16) |
                        ((uint32_t)rdc_buf[6] <<  8) |
                        ((uint32_t)rdc_buf[7]);
    uint32_t total    = ((uint32_t)rdc_buf[8]  << 24) |
                        ((uint32_t)rdc_buf[9]  << 16) |
                        ((uint32_t)rdc_buf[10] <<  8) |
                        ((uint32_t)rdc_buf[11]);

    if (blk_size == 0 || (blk_size & (blk_size - 1)) != 0 ||
        blk_size < 512U || blk_size > 4096U) {
        blk_size = 512U;
    }
    if (total == 0) total = 2097152U;

    out_geo->block_size   = blk_size;
    out_geo->total_blocks = total;
    return 0;
}

int dasd_fba_read_block(uint32_t schid, const dasd_fba_geo_t *geo,
                        uint32_t lba, void *buf, uint32_t len) {
    return dasd_fba_read_blocks(schid, geo, lba, 1, buf);
    (void)len;
}

int dasd_fba_read_blocks(uint32_t schid, const dasd_fba_geo_t *geo,
                         uint32_t lba_start, uint32_t count, void *buf) {
    if (count == 0) return 0;

    static fba_locate_arg_t locate_arg __attribute__((aligned(4)));
    static ccw1_t           chain[2]   __attribute__((aligned(8)));

    locate_arg.operation = FBA_LOCATE_OP_READ;
    locate_arg.auxiliary = 0;
    locate_arg.count     = (uint16_t)(count > 0xFFFFU ? 0xFFFFU : count);
    locate_arg.block_nr  = lba_start;

    chain[0].cmd   = FBA_CCW_LOCATE;
    chain[0].flags = CCW_FLAG_CC_SLI;
    chain[0].count = (uint16_t)sizeof(fba_locate_arg_t);
    chain[0].cda   = fba_cda(&locate_arg);

    const uint32_t total_bytes = count * geo->block_size;
    chain[1].cmd   = FBA_CCW_READ;
    chain[1].flags = CCW_FLAG_SLI;
    chain[1].count = (uint16_t)(total_bytes > 0xFFFFU ? 0xFFFFU : total_bytes);
    chain[1].cda   = fba_cda(buf);

    if (dasd_sync_io(schid, chain) < 0) {
        dasd_sense(schid);
        return -1;
    }
    return (int)count;
}
