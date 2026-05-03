// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_vtoc.h
//
/// @brief VTOC (Volume Table of Contents) search for the ZXFL bootloader.

#ifndef ZXFOUNDATION_ZXFL_DASD_VTOC_H
#define ZXFOUNDATION_ZXFL_DASD_VTOC_H

#include <zxfoundation/types.h>

/// @brief Parsed first extent from a Format-1 DSCB.
typedef struct {
    uint16_t begin_cyl;     ///< First cylinder of the extent
    uint16_t begin_head;    ///< First head (track within cylinder)
    uint16_t end_cyl;       ///< Last cylinder of the extent
    uint16_t end_head;      ///< Last head of the extent
} dscb1_extent_t;

/// Offsets when key IS present
#define DSCB1_KD_FMTID_OFF      44U         ///< DS1FMTID in key+data buffer
#define DSCB1_KD_EXTENT0_OFF    105U        ///< First extent in key+data buffer

/// Offsets when key is ABSENT (data-only buffer, 96 bytes):
#define DSCB1_D_FMTID_OFF       0U          ///< DS1FMTID in data-only buffer
#define DSCB1_D_EXTENT0_OFF     61U         ///< First extent in data-only buffer (105-44)

#define DSCB_FMT1_ID            0xF1U   ///< Format-1 DSCB (dataset entry)
#define DSCB_FMT4_ID            0xF4U   ///< Format-4 DSCB (VTOC header)
#define DSCB_FMT5_ID            0xF5U   ///< Format-5 DSCB (free space)

/// Default VTOC location — dasdload places the VTOC at cyl 0, head 1
#define VTOC_DEFAULT_CYL        0U
#define VTOC_DEFAULT_HEAD       1U

/// Maximum records to scan per VTOC track.
#define VTOC_MAX_RECORDS_PER_TRACK  20U

/// Maximum VTOC tracks to scan before giving up.
#define VTOC_MAX_TRACKS         5U

/// Maximum consecutive I/O failures before aborting the VTOC scan.
#define VTOC_MAX_CONSECUTIVE_ERRORS 3U

/// @brief Locate a dataset in the VTOC by ASCII name.
///
///        Scans the VTOC starting at (VTOC_DEFAULT_CYL, VTOC_DEFAULT_HEAD),
///        reading up to VTOC_MAX_TRACKS tracks and VTOC_MAX_RECORDS_PER_TRACK
///        records per track.
///
///        For each record, attempts Read Key+Data (0x0E) first.  If that
///        fails (unit check), issues SENSE to clear device state and retries
///        with Read Data (0x06) to handle 0-byte-key virtual DASD images.
///
///        Stops scanning when VTOC_MAX_CONSECUTIVE_ERRORS consecutive records
///        fail to read — this indicates the end of the VTOC, not a transient
///        error.  A single unreadable record does not abort the scan.
///
/// @param schid      Subchannel ID (from lowcore 0xB8 at IPL)
/// @param dsname     ASCII dataset name, NUL-terminated (e.g. "core.zxfoundation.nucleus")
/// @param out_extent Filled with the first extent descriptor on success
/// @return 0 on success, -1 if the dataset was not found
int dasd_find_dataset(uint32_t schid,
                      const char *dsname,
                      dscb1_extent_t *out_extent);

#endif /* ZXFOUNDATION_ZXFL_DASD_VTOC_H */
