// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/dasd_vtoc.c
//
/// @brief VTOC dataset search for the ZXFL bootloader.

#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/ebcdic.h>
#include <arch/s390x/init/zxfl/string.h>

/// @brief Read the VOL1 label at cyl 0, head 0, record 3 and extract the VTOC pointer.
/// @param schid    Subchannel ID
/// @param out_cyl  VTOC start cylinder
/// @param out_head VTOC start head
static void vtoc_find_from_vol1(uint32_t schid,
                                 uint16_t *out_cyl, uint16_t *out_head) {
    static uint8_t vol1[80] __attribute__((aligned(4)));
    
    *out_cyl  = VTOC_DEFAULT_CYL;
    *out_head = VTOC_DEFAULT_HEAD;

    // Read VOL1 label (always at C=0, H=0, R=3)
    int rc = dasd_read_record(schid, 0, 0, 3, CCW_CMD_READ_DATA, vol1, 80U);
    if (rc == 0 && vol1[0] == 0xE5 && vol1[1] == 0xD6 && vol1[2] == 0xD3 && vol1[3] == 0xF1) {
        // VOL1 label found. VTOC pointer is at offset 11.
        *out_cyl  = (uint16_t)((vol1[11] << 8) | vol1[12]);
        *out_head = (uint16_t)((vol1[13] << 8) | vol1[14]);
    } else {
        // Fallback to searching for Format-4 DSCB at the default location
        static uint8_t f4_buf[96] __attribute__((aligned(4)));
        rc = dasd_read_record(schid, 0, 1, 1, CCW_CMD_READ_KD, f4_buf, 96U);
        if (rc != 0) {
            dasd_sense(schid);
            rc = dasd_read_record(schid, 0, 1, 1, CCW_CMD_READ_DATA, f4_buf, 96U);
        }
        if (rc == 0 && f4_buf[0] == DSCB_FMT4_ID) {
            *out_cyl  = (uint16_t)((f4_buf[1] << 8) | f4_buf[2]);
            *out_head = (uint16_t)((f4_buf[3] << 8) | f4_buf[4]);
        }
    }
}

/// @brief Parse a raw DSCB buffer and check whether it matches dsname_ebcdic.
///
///        Key+data layout (buf_len=140):
///          buf[0..43]   = DS1DSNAM (44-byte EBCDIC name, space-padded 0x40)
///          buf[44]      = DS1FMTID (0xF1 for Format-1)
///          buf[108..117]= DS1EXTENT[0] (first extent, 10 bytes)
///            extent[2..3] = XTBCYL (begin cylinder, big-endian)
///            extent[4..5] = XTBTRK (begin head, big-endian)
///            extent[6..7] = XTECYL (end cylinder, big-endian)
///            extent[8..9] = XTETRK (end head, big-endian)
///
///        Data-only layout (buf_len=96, 0-byte key):
///          buf[0..43]   = DS1DSNAM
///          buf[44]      = DS1FMTID
///          buf[64..73]  = DS1EXTENT[0]
///
/// @return 1=match, 0=no match (skip), -1=unrecognised
static int dscb_match(uint8_t *buf, uint32_t buf_len,
                      uint8_t *dsname_ebcdic,
                      dscb1_extent_t *out_extent) {
    uint32_t extent_off;

    if (buf_len >= 140U && buf[DSCB1_KD_FMTID_OFF] == DSCB_FMT1_ID) {
        extent_off = DSCB1_KD_EXTENT0_OFF;
    } else if (buf_len >= 96U && buf[DSCB1_D_FMTID_OFF] == DSCB_FMT1_ID) {
        extent_off = DSCB1_D_EXTENT0_OFF;
    } else {
        return 0; // F4, F5, empty, or unrecognised — not an error
    }

    if (zxfl_memcmp(buf, dsname_ebcdic, 44U) != 0) return 0;

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
    uint8_t target[44];
    zxfl_memset(target, 0x40U, 44U);
    for (int i = 0; dsname[i] != '\0' && i < 44; i++)
        target[i] = ascii_to_ebcdic((uint8_t)dsname[i]);

    static uint8_t dscb_buf[140] __attribute__((aligned(4)));

    uint16_t cur_cyl  = VTOC_DEFAULT_CYL;
    uint16_t cur_head = VTOC_DEFAULT_HEAD;
    uint32_t consec   = 0;

    vtoc_find_from_vol1(schid, &cur_cyl, &cur_head);

    for (uint32_t track = 0; track < VTOC_MAX_TRACKS; track++) {
        for (uint8_t rec = 1; rec <= VTOC_MAX_RECORDS_PER_TRACK; rec++) {
            int      rc      = -1;
            uint32_t buf_len = 0;

            rc = dasd_read_record(schid, cur_cyl, cur_head, rec,
                                  CCW_CMD_READ_KD, dscb_buf, 140U);
            if (rc == 0) {
                buf_len = 140U;
                consec  = 0;
            } else {
                dasd_sense(schid);
                rc = dasd_read_record(schid, cur_cyl, cur_head, rec,
                                      CCW_CMD_READ_DATA, dscb_buf, 96U);
                if (rc == 0) {
                    buf_len = 96U;
                    consec  = 0;
                } else {
                    dasd_sense(schid);
                    consec++;

                    if (consec >= VTOC_MAX_CONSECUTIVE_ERRORS)
                        return -1;
                    continue;
                }
            }
            int match = dscb_match(dscb_buf, buf_len, target, out_extent);
            if (match == 1) return 0;
        }

        cur_head++;
        if (cur_head >= DASD_3390_HEADS_PER_CYL) {
            cur_head = 0;
            cur_cyl++;
        }
        consec = 0;
    }

    return -1;
}
