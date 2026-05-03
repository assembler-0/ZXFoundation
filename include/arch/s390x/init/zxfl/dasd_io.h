// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_io.h

#ifndef ZXFOUNDATION_ZXFL_DASD_IO_H
#define ZXFOUNDATION_ZXFL_DASD_IO_H

#include <zxfoundation/types.h>

/// @brief Channel Command Word, Format 1.
///        Must be 8-byte aligned as required by the channel subsystem.
typedef struct __attribute__((packed, aligned(8))) {
    uint8_t  cmd;       ///< CCW command code
    uint8_t  flags;     ///< CC (0x40), SLI (0x20), SKIP (0x10), PCI (0x08), IDA (0x04)
    uint16_t count;     ///< Byte count for data transfer
    uint32_t cda;       ///< Channel Data Address (31-bit physical)
} ccw1_t;

/// CCW command codes for 3390 DASD
#define CCW_CMD_SEEK            0x07U   ///< Seek to cylinder/head
#define CCW_CMD_SEARCH_ID_EQ    0x31U   ///< Search ID Equal (CCHHR match)
#define CCW_CMD_TIC             0x08U   ///< Transfer In Channel (loop back)
#define CCW_CMD_READ_DATA       0x06U   ///< Read Data (data field only)
#define CCW_CMD_READ_KD         0x0EU   ///< Read Key and Data
#define CCW_CMD_SENSE           0x04U   ///< Basic Sense (clears unit-check)

/// CCW flag bits
#define CCW_FLAG_CC             0x40U   ///< Command Chain
#define CCW_FLAG_SLI            0x20U   ///< Suppress Length Indication
#define CCW_FLAG_CC_SLI         (CCW_FLAG_CC | CCW_FLAG_SLI)

/// @brief ORB passed to SSCH.  4-byte aligned per PoP.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t intparm;       ///< Interruption parameter (returned in IRB)
    uint32_t flags;         ///< Key(4), S, C, M, Y, F(fmt1), P, I, A, U, LPM(8), PNOM, RSVD
    uint32_t cpa;           ///< Channel Program Address (first CCW, 31-bit)
    uint8_t  amsw[4];       ///< Address-Modifier Subword (reserved, 0)
    uint32_t reserved[4];
} orb_t;

/// ORB flag: Format-1 CCWs, LPM=0xFF (all paths), no prefetch
#define ORB_FLAGS_F1_LPM_FF     0x0080FF00U

/// @brief Seek argument: 6 bytes, big-endian CCHH.
typedef struct __attribute__((packed)) {
    uint16_t reserved;  ///< Must be 0
    uint16_t cyl;       ///< Cylinder number (big-endian)
    uint16_t head;      ///< Head number (big-endian)
} dasd_seek_arg_t;

/// @brief Search ID Equal argument: 5 bytes, big-endian CCHHR.
typedef struct __attribute__((packed)) {
    uint16_t cyl;       ///< Cylinder number
    uint16_t head;      ///< Head number
    uint8_t  rec;       ///< Record number (1-based)
} dasd_search_arg_t;

#define DASD_3390_HEADS_PER_CYL 15U     ///< Heads per cylinder on a 3390
#define DASD_3390_BYTES_PER_TRK 56664U  ///< Usable bytes per track (3390)
#define DASD_BLOCK_SIZE         4096U   ///< Fixed-block record size used by ZXFL

/// @brief Execute a synchronous SSCH/TSCH I/O operation.
///
///        Spins on TSCH (CC=1 = status not yet available) until the
///        operation completes.  The bootloader runs with interrupts
///        disabled, so polling is the only option.
///
/// @param schid  Subchannel identifier (loaded from lowcore 0xB8 at IPL)
/// @param ccw    Pointer to the first CCW in the channel program
/// @return 0 on success, -1 on channel/device error
int dasd_sync_io(uint32_t schid, ccw1_t *ccw);

/// @brief Issue a Basic Sense (0x04) to clear a unit-check condition.
///
///        A 3390 in unit-check state rejects all subsequent commands with
///        CMDREJ until sense data is collected.  We discard the sense bytes;
///        we only need the side-effect of clearing the condition.
///
/// @param schid  Subchannel identifier
void dasd_sense(uint32_t schid);

/// @brief Read a single DASD record (key+data or data-only).
///
///        Builds a 4-CCW SEEK/SEARCH/TIC/READ chain and executes it
///        synchronously.  Static storage is used for the CCW chain and
///        seek/search arguments to avoid stack pressure.
///
/// @param schid    Subchannel ID
/// @param cyl      Cylinder number
/// @param head     Head number
/// @param rec      Record number (1-based)
/// @param rd_cmd   CCW_CMD_READ_DATA or CCW_CMD_READ_KD
/// @param buf      Destination buffer (must be 4-byte aligned)
/// @param len      Buffer length in bytes
/// @return 0 on success, -1 on error
int dasd_read_record(uint32_t schid,
                     uint16_t cyl, uint16_t head, uint8_t rec,
                     uint8_t rd_cmd, void *buf, uint32_t len);

/// @brief Read a DASD record and advance the position to the next record.
///
///        Wraps dasd_read_record and increments (cyl, head, rec) after a
///        successful read, handling track and cylinder wrap-around.
///
/// @param schid    Subchannel ID
/// @param cyl      In/out: cylinder number
/// @param head     In/out: head number
/// @param rec      In/out: record number (1-based)
/// @param rd_cmd   CCW_CMD_READ_DATA or CCW_CMD_READ_KD
/// @param buf      Destination buffer
/// @param len      Buffer length in bytes
/// @return 0 on success, -1 on error
int dasd_read_next(uint32_t schid,
                   uint16_t *cyl, uint16_t *head, uint8_t *rec,
                   uint8_t rd_cmd, void *buf, uint32_t len);

#endif /* ZXFOUNDATION_ZXFL_DASD_IO_H */
