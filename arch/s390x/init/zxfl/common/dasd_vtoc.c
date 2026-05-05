// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/dasd_vtoc.c
//
/// @brief VTOC dataset search for the ZXFL bootloader.
///
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/ebcdic.h>
#include <arch/s390x/init/zxfl/string.h>

/// @brief Parse a 10-byte IBM extent descriptor into dscb1_extent_t.
///        IBM extent layout (PoP DSCB1 DS1EXTENT):
///          byte 0:   extent type (0x00 = unused, 0x01 = cylinder, etc.)
///          byte 1:   sequence number
///          bytes 2-3: XTBCYL (begin cylinder, big-endian)
///          bytes 4-5: XTBTRK (begin head, big-endian)
///          bytes 6-7: XTECYL (end cylinder, big-endian)
///          bytes 8-9: XTETRK (end head, big-endian)
static bool parse_extent(const uint8_t *e, dscb1_extent_t *out) {
    // Extent type 0x00 means unused — skip.
    if (e[0] == 0x00) return false;
    out->begin_cyl  = (uint16_t)((e[2] << 8) | e[3]);
    out->begin_head = (uint16_t)((e[4] << 8) | e[5]);
    out->end_cyl    = (uint16_t)((e[6] << 8) | e[7]);
    out->end_head   = (uint16_t)((e[8] << 8) | e[9]);
    return true;
}

/// @brief Read the VOL1 label and extract the VTOC start CCHH.
///        Falls back to (0,1) if the label is absent or unreadable.
static void vtoc_find_from_vol1(uint32_t schid,
                                uint16_t *out_cyl, uint16_t *out_head) {
    static uint8_t vol1[80] __attribute__((aligned(4)));

    *out_cyl  = VTOC_DEFAULT_CYL;
    *out_head = VTOC_DEFAULT_HEAD;

    if (dasd_read_record(schid, 0, 0, 3, CCW_CMD_READ_DATA, vol1, 80U) != 0)
        return;

    // VOL1 EBCDIC label: bytes 0-3 = 0xE5 0xD6 0xD3 0xF1 ("VOL1")
    if (vol1[0] != 0xE5U || vol1[1] != 0xD6U ||
        vol1[2] != 0xD3U || vol1[3] != 0xF1U)
        return;

    // VTOC pointer at offset 11: CCHH (4 bytes, big-endian)
    *out_cyl  = (uint16_t)((vol1[11] << 8) | vol1[12]);
    *out_head = (uint16_t)((vol1[13] << 8) | vol1[14]);
}

/// @brief Read a DSCB record, trying key+data first then data-only.
///        Returns the buffer length used (140 or 96), or 0 on failure.
static uint32_t read_dscb(uint32_t schid, uint16_t cyl, uint16_t head,
                           uint8_t rec, uint8_t *buf) {
    if (dasd_read_record(schid, cyl, head, rec,
                         CCW_CMD_READ_KD, buf, 140U) == 0)
        return 140U;
    dasd_sense(schid);
    if (dasd_read_record(schid, cyl, head, rec,
                         CCW_CMD_READ_DATA, buf, 96U) == 0)
        return 96U;
    dasd_sense(schid);
    return 0;
}

/// @brief Follow the DSCB3 chain starting at (cyl, head, rec) and append
///        extents to @p ds until the chain ends or VTOC_MAX_EXTENTS is full.
static void collect_dscb3_chain(uint32_t schid,
                                uint16_t cyl, uint16_t head, uint8_t rec,
                                dasd_dataset_t *ds) {
    static uint8_t buf[140] __attribute__((aligned(4)));

    // Guard against infinite loops from corrupt VTOC.
    for (uint32_t depth = 0; depth < 8U && ds->count < VTOC_MAX_EXTENTS; depth++) {
        if (cyl == 0 && head == 0 && rec == 0) break;

        uint32_t len = read_dscb(schid, cyl, head, rec, buf);
        if (len == 0) break;

        // Validate DSCB3 format ID.
        // Key+data: fmtid at buf[5]; data-only: fmtid at buf[0] (key stripped).
        uint8_t fmtid = (len == 140U) ? buf[DSCB3_KD_FMTID_OFF]
                                      : buf[0];
        if (fmtid != DSCB_FMT3_ID) break;

        uint32_t ext_base = (len == 140U) ? DSCB3_KD_EXTENT0_OFF : 1U;
        uint32_t ptr_base = (len == 140U) ? DSCB3_KD_PTR_OFF
                                          : (DSCB3_KD_PTR_OFF - 5U);

        for (uint32_t i = 0; i < DSCB3_KD_EXTENTS && ds->count < VTOC_MAX_EXTENTS; i++) {
            dscb1_extent_t ext;
            if (parse_extent(buf + ext_base + i * 10U, &ext))
                ds->extents[ds->count++] = ext;
        }

        // Next DSCB3 pointer: CCHHR (5 bytes).
        if (ptr_base + 5U > len) break;
        cyl  = (uint16_t)((buf[ptr_base + 0] << 8) | buf[ptr_base + 1]);
        head = (uint16_t)((buf[ptr_base + 2] << 8) | buf[ptr_base + 3]);
        rec  = buf[ptr_base + 4];
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int dasd_find_dataset_extents(uint32_t schid,
                              const char *dsname,
                              dasd_dataset_t *out_ds) {
    // Convert ASCII dataset name to 44-byte space-padded EBCDIC.
    uint8_t target[44];
    memset(target, 0x40U, 44U);
    for (int i = 0; dsname[i] != '\0' && i < 44; i++)
        target[i] = ascii_to_ebcdic((uint8_t)dsname[i]);

    static uint8_t dscb_buf[140] __attribute__((aligned(4)));

    uint16_t cur_cyl  = VTOC_DEFAULT_CYL;
    uint16_t cur_head = VTOC_DEFAULT_HEAD;
    uint32_t consec   = 0;

    vtoc_find_from_vol1(schid, &cur_cyl, &cur_head);

    for (uint32_t track = 0; track < VTOC_MAX_TRACKS; track++) {
        for (uint8_t rec = 1; rec <= VTOC_MAX_RECORDS_PER_TRACK; rec++) {
            uint32_t buf_len = read_dscb(schid, cur_cyl, cur_head, rec, dscb_buf);
            if (buf_len == 0) {
                if (++consec >= VTOC_MAX_CONSECUTIVE_ERRORS) return -1;
                continue;
            }
            consec = 0;

            // Determine FMTID and name offset based on key presence.
            uint8_t  fmtid;
            uint32_t name_off;
            uint32_t ext0_off;
            uint32_t ptr3_off;

            if (buf_len == 140U) {
                fmtid    = dscb_buf[DSCB1_KD_FMTID_OFF];
                name_off = 0U;
                ext0_off = DSCB1_KD_EXTENT0_OFF;
                ptr3_off = DSCB1_KD_PTR3_OFF;
            } else {
                fmtid    = dscb_buf[DSCB1_D_FMTID_OFF];
                name_off = 0U;
                ext0_off = DSCB1_D_EXTENT0_OFF;
                ptr3_off = DSCB1_D_PTR3_OFF;
            }

            // Skip non-DSCB1 records (F4=VTOC header, F5=free space, etc.)
            if (fmtid != DSCB_FMT1_ID) continue;

            // Compare 44-byte EBCDIC dataset name.
            if (memcmp(dscb_buf + name_off, target, 44U) != 0) continue;

            // Match found — collect up to 3 extents from DSCB1.
            out_ds->count = 0;
            for (uint32_t i = 0; i < 3U && out_ds->count < VTOC_MAX_EXTENTS; i++) {
                dscb1_extent_t ext;
                if (parse_extent(dscb_buf + ext0_off + i * 10U, &ext))
                    out_ds->extents[out_ds->count++] = ext;
            }

            // Follow DSCB3 chain if DS1PTRDS is non-zero.
            if (ptr3_off + 5U <= buf_len) {
                const uint8_t *ptr = dscb_buf + ptr3_off;
                uint16_t p_cyl  = (uint16_t)((ptr[0] << 8) | ptr[1]);
                uint16_t p_head = (uint16_t)((ptr[2] << 8) | ptr[3]);
                uint8_t  p_rec  = ptr[4];
                if (p_cyl != 0 || p_head != 0 || p_rec != 0)
                    collect_dscb3_chain(schid, p_cyl, p_head, p_rec, out_ds);
            }

            return (out_ds->count > 0) ? 0 : -1;
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

int dasd_find_dataset(uint32_t schid,
                      const char *dsname,
                      dscb1_extent_t *out_extent) {
    dasd_dataset_t ds;
    if (dasd_find_dataset_extents(schid, dsname, &ds) < 0) return -1;
    *out_extent = ds.extents[0];
    return 0;
}
