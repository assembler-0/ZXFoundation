// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd_vtoc.h
//
/// @brief VTOC (Volume Table of Contents) search for the ZXFL bootloader.
///
///        Supports:
///          - VOL1 label parsing to locate the VTOC
///          - Format-1 DSCB: up to 3 extents per DSCB1
///          - Format-3 DSCB chaining: up to 13 additional extents per DSCB3
///          - Format-5 DSCB: free-space awareness (skipped, not needed for read)
///          - Fallback to Format-4 DSCB at default location

#ifndef ZXFOUNDATION_ZXFL_DASD_VTOC_H
#define ZXFOUNDATION_ZXFL_DASD_VTOC_H

#include <zxfoundation/types.h>

/// @brief One DASD extent (begin/end CCHH).
typedef struct {
    uint16_t begin_cyl;
    uint16_t begin_head;
    uint16_t end_cyl;
    uint16_t end_head;
} dscb1_extent_t;

/// @brief Maximum extents we will collect for a single dataset.
///        DSCB1 holds 3, each DSCB3 holds 13.  One DSCB3 chain gives 16 total.
///        Two DSCB3s give 29.  We cap at 16 for static allocation.
#define VTOC_MAX_EXTENTS    16U

/// @brief Full extent list for a dataset (DSCB1 + DSCB3 chain).
typedef struct {
    dscb1_extent_t extents[VTOC_MAX_EXTENTS];
    uint32_t       count;   ///< Number of valid entries in extents[]
} dasd_dataset_t;

/// Offsets when key IS present (key+data buffer, 140 bytes total):
///   buf[0..43]   = DS1DSNAM (44-byte EBCDIC name)
///   buf[44]      = DS1FMTID
///   buf[105..114]= DS1EXTENT[0] (10 bytes each, 3 extents)
///   buf[135..136]= DS1PTRDS (CCHHR pointer to first DSCB3, 5 bytes at 135)
#define DSCB1_KD_FMTID_OFF      44U
#define DSCB1_KD_EXTENT0_OFF    105U    ///< First of 3 extents (10 bytes each)
#define DSCB1_KD_PTR3_OFF       135U    ///< DSCB3 pointer (CCHHR, 5 bytes)

/// Offsets when key is ABSENT (data-only buffer, 96 bytes):
///   buf[0..43]   = DS1DSNAM
///   buf[44]      = DS1FMTID
///   buf[61..70]  = DS1EXTENT[0]
///   buf[91..95]  = DS1PTRDS (CCHHR, 5 bytes)
#define DSCB1_D_FMTID_OFF       0U
#define DSCB1_D_EXTENT0_OFF     61U
#define DSCB1_D_PTR3_OFF        91U

/// DSCB3 key+data layout (140 bytes):
///   buf[0..4]    = DS3KEYID (5-byte key: CCHHR of this DSCB3)
///   buf[5]       = DS3FMTID (0xF3)
///   buf[6..135]  = DS3EXTNT: 13 extents × 10 bytes
///   buf[136..140]= DS3PTRDS: CCHHR pointer to next DSCB3 (0 = end of chain)
#define DSCB3_KD_FMTID_OFF      5U
#define DSCB3_KD_EXTENT0_OFF    6U      ///< 13 extents × 10 bytes
#define DSCB3_KD_PTR_OFF        136U    ///< Next DSCB3 CCHHR (5 bytes)
#define DSCB3_KD_EXTENTS        13U

/// DSCB format IDs
#define DSCB_FMT1_ID            0xF1U
#define DSCB_FMT3_ID            0xF3U
#define DSCB_FMT4_ID            0xF4U
#define DSCB_FMT5_ID            0xF5U

#define VTOC_DEFAULT_CYL                0U
#define VTOC_DEFAULT_HEAD               1U
#define VTOC_MAX_RECORDS_PER_TRACK      20U
#define VTOC_MAX_TRACKS                 30U
#define VTOC_MAX_CONSECUTIVE_ERRORS     3U

/// @brief Locate a dataset in the VTOC and collect all its extents.
///
///        Scans the VTOC for a Format-1 DSCB matching @p dsname, then
///        follows the DSCB3 chain to collect all extents into @p out_ds.
///
/// @param schid    Subchannel ID
/// @param dsname   ASCII dataset name, NUL-terminated
/// @param out_ds   Receives the full extent list on success
/// @return 0 on success, -1 if not found
int dasd_find_dataset_extents(uint32_t schid,
                              const char *dsname,
                              dasd_dataset_t *out_ds);

/// @brief Locate a dataset and return only its first extent.
///
///        Convenience wrapper over dasd_find_dataset_extents() for callers
///        that only need the first extent (e.g. stage1, elfload).
///
/// @param schid      Subchannel ID
/// @param dsname     ASCII dataset name, NUL-terminated
/// @param out_extent Receives the first extent on success
/// @return 0 on success, -1 if not found
int dasd_find_dataset(uint32_t schid,
                      const char *dsname,
                      dscb1_extent_t *out_extent);

#endif /* ZXFOUNDATION_ZXFL_DASD_VTOC_H */
