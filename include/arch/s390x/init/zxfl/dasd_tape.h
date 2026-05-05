// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_tape.h
//
/// @brief CCW-based sequential tape driver for ZXFL.

#ifndef ZXFOUNDATION_ZXFL_DASD_TAPE_H
#define ZXFOUNDATION_ZXFL_DASD_TAPE_H

#include <zxfoundation/types.h>

#define TAPE_CCW_REWIND         0x01U   ///< Rewind to beginning of tape
#define TAPE_CCW_READ_FWD       0x02U   ///< Read Forward (one block)
#define TAPE_CCW_READ_BACKWARD  0x0CU   ///< Read Backward (one block)
#define TAPE_CCW_WRITE          0x01U   ///< Write (not used in bootloader)
#define TAPE_CCW_WTM            0x1FU   ///< Write Tape Mark
#define TAPE_CCW_FSF            0x3BU   ///< Forward Space File (skip tape mark)
#define TAPE_CCW_BSF            0x3FU   ///< Backward Space File
#define TAPE_CCW_FSR            0x37U   ///< Forward Space Record
#define TAPE_CCW_BSR            0x27U   ///< Backward Space Record
#define TAPE_CCW_SENSE          0x04U   ///< Basic Sense
#define TAPE_CCW_SENSE_ID       0xE4U   ///< Sense ID

#define TAPE_DEV_TYPE_3480      0x3480U
#define TAPE_DEV_TYPE_3490      0x3490U
#define TAPE_DEV_TYPE_3590      0x3590U

/// Unit Exception in the IRB device status byte — signals a tape mark.
/// When READ FORWARD hits a tape mark, the channel posts UE without error.
/// The bootloader treats this as end-of-file.
#define TAPE_IRB_UNIT_EXCEPTION 0x00010000U   ///< Bit 15 of IRB word 2 (device status)

/// @brief Tape device state, populated by dasd_tape_probe().
typedef struct {
    uint16_t dev_type;      ///< TAPE_DEV_TYPE_* constant
    uint16_t dev_model;     ///< Device model byte from Sense ID
    uint32_t max_block;     ///< Maximum block size in bytes (32768 for 3590)
} dasd_tape_geo_t;

/// @brief Probe a tape device via Sense ID.
///
///        Verifies the device is a 3480/3490/3590 and populates @p out_geo.
///        Does NOT rewind — call dasd_tape_rewind() before reading.
///
/// @param schid    Subchannel ID
/// @param out_geo  Output descriptor
/// @return 0 on success, -1 if not a supported tape device
int dasd_tape_probe(uint32_t schid, dasd_tape_geo_t *out_geo);

/// @brief Rewind the tape to beginning-of-tape (BOT).
///
///        Issues the REWIND CCW and waits for completion.  Must be called
///        before the first dasd_tape_read_block() after IPL, because the
///        tape position is undefined at power-on.
///
/// @param schid    Subchannel ID
/// @return 0 on success, -1 on I/O error
int dasd_tape_rewind(uint32_t schid);

/// @brief Read one block from the tape at the current position.
///
///        Issues READ FORWARD.  The tape drive advances past the block.
///        If the drive encounters a tape mark instead of data, returns
///        TAPE_EOF (1) — the caller should treat this as end-of-file.
///        The buffer is not modified on EOF.
///
/// @param schid    Subchannel ID
/// @param buf      Destination buffer (must be 4-byte aligned, < 2 GB physical)
/// @param len      Buffer length in bytes (must be >= block size on tape)
/// @param out_read Actual bytes transferred (may be less than len for short blocks)
/// @return 0 on success, 1 on tape mark (EOF), -1 on I/O error
int dasd_tape_read_block(uint32_t schid, void *buf, uint32_t len,
                         uint32_t *out_read);

/// @brief Skip forward over one tape mark (file separator).
///
///        Issues Forward Space File.  Used to position past a tape mark
///        without reading data.
///
/// @param schid    Subchannel ID
/// @return 0 on success, -1 on I/O error
int dasd_tape_fsf(uint32_t schid);

/// @brief Read an entire tape file (sequence of blocks terminated by a tape mark)
///        into a contiguous buffer.
///
///        Calls dasd_tape_read_block() in a loop until EOF or @p max_bytes
///        is exhausted.  The tape is left positioned after the tape mark.
///
/// @param schid      Subchannel ID
/// @param buf        Destination buffer
/// @param max_bytes  Maximum bytes to read
/// @param out_size   Total bytes actually read
/// @return 0 on success (tape mark reached), -1 on I/O error
int dasd_tape_read_file(uint32_t schid, void *buf, uint32_t max_bytes,
                        uint32_t *out_size);

#define TAPE_EOF    1   ///< Returned by dasd_tape_read_block on tape mark

#endif /* ZXFOUNDATION_ZXFL_DASD_TAPE_H */
