// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_fba.h
//
/// @brief FBA (Fixed Block Architecture) DASD driver for ZXFL.

#ifndef ZXFOUNDATION_ZXFL_DASD_FBA_H
#define ZXFOUNDATION_ZXFL_DASD_FBA_H

#include <zxfoundation/types.h>

#define FBA_CCW_LOCATE          0x43U   ///< Locate: set starting block + count
#define FBA_CCW_READ            0x42U   ///< Read: transfer blocks into buffer
#define FBA_CCW_WRITE           0x41U   ///< Write: transfer buffer to device
#define FBA_CCW_SENSE_ID        0xE4U   ///< Sense ID (same as ECKD)
#define FBA_CCW_RDC             0x64U   ///< Read Device Characteristics

#define FBA_DEV_TYPE_9336       0x9336U
#define FBA_DEV_TYPE_0671       0x0671U   ///< SCSI-emulated FBA

#define FBA_LOCATE_OP_READ      0x06U   ///< Read operation
#define FBA_LOCATE_OP_WRITE     0x05U   ///< Write operation

/// @brief FBA device geometry as probed from the hardware.
typedef struct {
    uint16_t dev_type;          ///< Device type (e.g. FBA_DEV_TYPE_9336)
    uint16_t dev_model;         ///< Device model
    uint32_t block_size;        ///< Block size in bytes (512 or 4096)
    uint32_t total_blocks;      ///< Total addressable blocks on the device
} dasd_fba_geo_t;

/// @brief LOCATE argument block (8 bytes, big-endian).
///        Passed as the data area for the LOCATE CCW.
typedef struct __attribute__((packed)) {
    uint8_t  operation;     ///< FBA_LOCATE_OP_READ or FBA_LOCATE_OP_WRITE
    uint8_t  auxiliary;     ///< Auxiliary byte (0 for normal I/O)
    uint16_t count;         ///< Number of blocks to transfer
    uint32_t block_nr;      ///< Starting block number (0-based, big-endian)
} fba_locate_arg_t;

/// @brief Probe an FBA device and populate geometry.
///
///        Issues Sense ID and Read Device Characteristics to determine
///        the device type, block size, and total block count.
///
/// @param schid    Subchannel ID
/// @param out_geo  Output geometry structure
/// @return 0 on success, -1 if the device is not an FBA device or I/O fails
int dasd_fba_probe(uint32_t schid, dasd_fba_geo_t *out_geo);

/// @brief Read a single block from an FBA device by LBA.
///
/// @param schid    Subchannel ID
/// @param geo      Device geometry (from dasd_fba_probe)
/// @param lba      Linear block address (0-based)
/// @param buf      Destination buffer (must be 4-byte aligned, < 2 GB physical)
/// @param len      Buffer length (must equal geo->block_size)
/// @return 0 on success, -1 on I/O error
int dasd_fba_read_block(uint32_t schid, const dasd_fba_geo_t *geo,
                        uint32_t lba, void *buf, uint32_t len);

/// @brief Read a contiguous range of blocks from an FBA device.
///
///        Issues a single LOCATE + READ CCW chain for the entire range,
///        which is more efficient than issuing one chain per block.
///
/// @param schid      Subchannel ID
/// @param geo        Device geometry
/// @param lba_start  First LBA to read
/// @param count      Number of blocks to read
/// @param buf        Destination buffer (must hold count × geo->block_size bytes)
/// @return Number of blocks successfully read, or -1 on I/O error
int dasd_fba_read_blocks(uint32_t schid, const dasd_fba_geo_t *geo,
                         uint32_t lba_start, uint32_t count, void *buf);

#endif /* ZXFOUNDATION_ZXFL_DASD_FBA_H */
