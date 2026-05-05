// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_eckd.h
//
/// @brief ECKD (Extended Count Key Data) DASD driver for ZXFL.
///
///        ECKD is the native format for IBM 3390 DASD.  Unlike FBA, ECKD
///        exposes the physical geometry (cylinder/head/record) directly.
///        This driver wraps the raw dasd_io CCW engine with geometry-aware
///        LBA translation and device probing via Sense ID / Read Device
///        Characteristics.

#ifndef ZXFOUNDATION_ZXFL_DASD_ECKD_H
#define ZXFOUNDATION_ZXFL_DASD_ECKD_H

#include <zxfoundation/types.h>

#define ECKD_DEV_TYPE_3390      0x3390U
#define ECKD_DEV_TYPE_3380      0x3380U
#define ECKD_DEV_TYPE_9345      0x9345U

#define ECKD_3390_HEADS         15U         ///< Heads per cylinder
#define ECKD_3390_RECS_PER_TRK  12U         ///< Fixed-block records per track (4096-byte blocks)
#define ECKD_3390_BYTES_PER_TRK 56664U      ///< Usable bytes per track

#define CCW_CMD_SENSE_ID        0xE4U

#define CCW_CMD_RDC             0x64U

/// @brief ECKD device geometry as probed from the hardware.
typedef struct {
    uint16_t dev_type;          ///< Device type (e.g. ECKD_DEV_TYPE_3390)
    uint16_t dev_model;         ///< Device model (e.g. 0x0C for 3390-12)
    uint16_t cyls;              ///< Total cylinders
    uint16_t heads;             ///< Heads per cylinder
    uint16_t recs_per_trk;      ///< Fixed-block records per track
    uint32_t block_size;        ///< Block size in bytes (always 4096 for ZXFL)
    uint32_t total_blocks;      ///< Total addressable blocks
} dasd_eckd_geo_t;

/// @brief Probe an ECKD device and populate geometry.
///
///        Issues Sense ID and Read Device Characteristics to determine
///        the device type, model, and geometry.  Falls back to 3390
///        defaults if RDC fails.
///
/// @param schid    Subchannel ID
/// @param out_geo  Output geometry structure
/// @return 0 on success, -1 if the device is not an ECKD device or I/O fails
int dasd_eckd_probe(uint32_t schid, dasd_eckd_geo_t *out_geo);

/// @brief Translate a linear block address to ECKD CCHHR.
///
/// @param geo      Device geometry (from dasd_eckd_probe)
/// @param lba      Linear block address (0-based)
/// @param out_cyl  Output cylinder
/// @param out_head Output head
/// @param out_rec  Output record (1-based)
void dasd_eckd_geo_from_lba(const dasd_eckd_geo_t *geo, uint32_t lba,
                             uint16_t *out_cyl, uint16_t *out_head,
                             uint8_t  *out_rec);

/// @brief Read a single block from an ECKD device by LBA.
///
/// @param schid    Subchannel ID
/// @param geo      Device geometry
/// @param lba      Linear block address
/// @param buf      Destination buffer (must be 4-byte aligned, < 2 GB physical)
/// @param len      Buffer length (must equal geo->block_size)
/// @return 0 on success, -1 on I/O error
int dasd_eckd_read_block(uint32_t schid, const dasd_eckd_geo_t *geo,
                         uint32_t lba, void *buf, uint32_t len);

/// @brief Read a contiguous range of blocks from an ECKD device.
///
///        Reads [lba_start, lba_start + count) into buf.  Each block is
///        geo->block_size bytes.  The caller must ensure buf is large enough.
///
/// @param schid      Subchannel ID
/// @param geo        Device geometry
/// @param lba_start  First LBA to read
/// @param count      Number of blocks to read
/// @param buf        Destination buffer
/// @return Number of blocks successfully read, or -1 on the first I/O error
int dasd_eckd_read_blocks(uint32_t schid, const dasd_eckd_geo_t *geo,
                          uint32_t lba_start, uint32_t count, void *buf);

#endif /* ZXFOUNDATION_ZXFL_DASD_ECKD_H */
