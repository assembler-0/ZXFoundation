// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd.h
//
/// @brief DASD I/O primitives for the ZXFL bootloader.
///        Covers CCW/ORB construction, synchronous I/O, and VTOC search.
///        All structures are freestanding-safe (no libc dependency).

#ifndef ZXFOUNDATION_ZXFL_DASD_H
#define ZXFOUNDATION_ZXFL_DASD_H

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// CCW Format-1 (used when ORB.flags has the F bit set, 0x0080xxxx)
// ---------------------------------------------------------------------------
/// @brief Channel Command Word, Format 1.
///        8-byte aligned as required by the channel subsystem.
typedef struct __attribute__((packed, aligned(8))) {
    uint8_t  cmd;       ///< CCW command code
    uint8_t  flags;     ///< CC (0x40), SLI (0x20), SKIP (0x10), PCI (0x08), IDA (0x04)
    uint16_t count;     ///< Byte count for data transfer
    uint32_t cda;       ///< Channel Data Address (31-bit physical)
} ccw1_t;

// CCW command codes for 3390 DASD
#define CCW_CMD_SEEK            0x07U   ///< Seek to cylinder/head
#define CCW_CMD_SEARCH_ID_EQ    0x31U   ///< Search ID Equal (CCHHR match)
#define CCW_CMD_TIC             0x08U   ///< Transfer In Channel (loop back)
#define CCW_CMD_READ_DATA       0x06U   ///< Read Data (data field only)
#define CCW_CMD_READ_KD         0x0EU   ///< Read Key and Data

// CCW flag bits
#define CCW_FLAG_CC             0x40U   ///< Command Chain
#define CCW_FLAG_SLI            0x20U   ///< Suppress Length Indication
#define CCW_FLAG_CC_SLI         (CCW_FLAG_CC | CCW_FLAG_SLI)

// ---------------------------------------------------------------------------
// Operation Request Block (ORB)
// ---------------------------------------------------------------------------
/// @brief ORB passed to SSCH.  4-byte aligned per PoP.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t intparm;   ///< Interruption parameter (returned in IRB)
    uint32_t flags;     ///< Key(4), S, C, M, Y, F(fmt1), P, I, A, U, LPM(8), PNOM, RSVD
    uint32_t cpa;       ///< Channel Program Address (first CCW, 31-bit)
    uint8_t  amsw[4];   ///< Address-Modifier Subword (reserved, 0)
    uint32_t reserved[4];
} orb_t;

// ORB flag: Format-1 CCWs, LPM=0xFF (all paths), no prefetch
#define ORB_FLAGS_F1_LPM_FF     0x0080FF00U

// ---------------------------------------------------------------------------
// DASD seek / search arguments
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// DSCB (Data Set Control Block) — Format 1
// The VTOC on a 3390 lives at the track specified by the Format-4 DSCB.
// We default to cyl 0, head 14 (track 14) per sysres.conf.
//
// Format-1 DSCB layout (key=44 bytes, data=96 bytes):
//   key[0..43]  : DS1DSNAM  — dataset name, EBCDIC, space-padded (0x40)
//   data[0]     : DS1FMTID  — format identifier ('1' = 0xF1 in EBCDIC)
//   data[1..43] : DS1DSSN, DS1VOLSQ, DS1CREDT, DS1EXPDT, DS1NOEPV,
//                 DS1NOBDB, DS1FLAG1, DS1SYSCD, DS1REFD, DS1SMSFG,
//                 DS1SCEXT, DS1SCALO, DS1LSTAR, DS1TRBAL
//   data[44..53]: DS1DSORG, DS1RECFM, DS1OPTCD, DS1BLKL, DS1LRECL,
//                 DS1KEYL, DS1RKP, DS1DSIND, DS1SCAL2
//   data[54..63]: DS1DSSEC (security)
//   data[64..73]: DS1EXTENT[0] — first extent descriptor (10 bytes)
//     extent[0]  : XTTYPE  — extent type (0x40 = data)
//     extent[1]  : XTSEQN  — extent sequence number
//     extent[2..3]: XTBCYL — begin cylinder (big-endian)
//     extent[4..5]: XTBTRK — begin head (big-endian)
//     extent[6..7]: XTECYL — end cylinder (big-endian)
//     extent[8..9]: XTETRK — end head (big-endian)
//
// When key length is 0 (special virtual disks), the hardware returns only
// the 96-byte data portion.  DS1DSNAM is still at data[0..43] and DS1FMTID
// is at data[44].  The extent is at data[64..73].
// ---------------------------------------------------------------------------

/// @brief Parsed extent from a Format-1 DSCB.
typedef struct {
    uint16_t begin_cyl;     ///< First cylinder of the extent
    uint16_t begin_head;    ///< First head (track within cylinder)
    uint16_t end_cyl;       ///< Last cylinder of the extent
    uint16_t end_head;      ///< Last head of the extent
} dscb1_extent_t;

// DSCB offsets when key IS present (key+data combined, 140 bytes total)
#define DSCB1_KD_FMTID_OFF      44U     ///< DS1FMTID in key+data buffer
#define DSCB1_KD_EXTENT0_OFF    (44U + 64U) ///< First extent in key+data buffer

// DSCB offsets when key is ABSENT (data-only, 96 bytes)
#define DSCB1_D_FMTID_OFF       44U     ///< DS1FMTID in data-only buffer
#define DSCB1_D_EXTENT0_OFF     64U     ///< First extent in data-only buffer

// EBCDIC '1' — Format-1 DSCB identifier
#define DSCB_FMT1_ID            0xF1U

// VTOC default location on a 3390-1 (15 heads/cyl): track 14 = cyl 0, head 14
#define VTOC_DEFAULT_CYL        0U
#define VTOC_DEFAULT_HEAD       14U

// Maximum records to scan per VTOC track
#define VTOC_MAX_RECORDS        100U

// ---------------------------------------------------------------------------
// 3390 geometry constants
// ---------------------------------------------------------------------------
#define DASD_3390_HEADS_PER_CYL 15U     ///< Heads per cylinder on a 3390
#define DASD_3390_BYTES_PER_TRK 56664U  ///< Usable bytes per track (3390)
#define DASD_BLOCK_SIZE         4096U   ///< Fixed-block record size used by ZXFL

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Execute a synchronous SSCH/TSCH I/O operation.
/// @param schid  Subchannel identifier (loaded from 0xB8 at IPL)
/// @param ccw    Pointer to the first CCW in the channel program
/// @return 0 on success, -1 on channel/device error
int dasd_sync_io(uint32_t schid, ccw1_t *ccw);

/// @brief Read a single DASD record (key+data or data-only).
/// @param schid    Subchannel ID
/// @param cyl      Cylinder number
/// @param head     Head number
/// @param rec      Record number (1-based)
/// @param rd_cmd   CCW_CMD_READ_DATA or CCW_CMD_READ_KD
/// @param buf      Destination buffer
/// @param len      Buffer length in bytes
/// @return 0 on success, -1 on error
int dasd_read_record(uint32_t schid,
                     uint16_t cyl, uint16_t head, uint8_t rec,
                     uint8_t rd_cmd, void *buf, uint32_t len);

/// @brief Read a DASD record advancing across track/cylinder boundaries.
/// @param schid    Subchannel ID
/// @param cyl      Starting cylinder (updated on return)
/// @param head     Starting head (updated on return)
/// @param rec      Starting record number (updated on return, 1-based)
/// @param rd_cmd   CCW_CMD_READ_DATA or CCW_CMD_READ_KD
/// @param buf      Destination buffer
/// @param len      Buffer length in bytes
/// @return 0 on success, -1 on error
int dasd_read_next(uint32_t schid,
                   uint16_t *cyl, uint16_t *head, uint8_t *rec,
                   uint8_t rd_cmd, void *buf, uint32_t len);

/// @brief Locate a dataset in the VTOC by name.
///        Handles both 44-byte-key and 0-byte-key DSCB layouts.
/// @param schid      Subchannel ID
/// @param dsname     ASCII dataset name (e.g. "SYS1.NUCLEUS")
/// @param out_extent Filled with the first extent on success
/// @return 0 on success, -1 if not found
int dasd_find_dataset(uint32_t schid,
                      const char *dsname,
                      dscb1_extent_t *out_extent);

#endif /* ZXFOUNDATION_ZXFL_DASD_H */
