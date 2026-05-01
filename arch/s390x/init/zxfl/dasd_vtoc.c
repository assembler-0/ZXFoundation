// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/dasd_vtoc.c
//
/// @brief VTOC dataset search for the ZXFL bootloader.

#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/ebcdic.h>
#include <arch/s390x/init/zxfl/diag.h>

static void vtoc_print_hex8(uint32_t v) {
    const char hex[] = "0123456789abcdef";
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
    for (int i = 0; i < 8; i++) diag_putc(buf[i]);
}

static void vtoc_print_hex2(uint8_t v) {
    const char hex[] = "0123456789abcdef";
    diag_putc(hex[(v >> 4) & 0xF]);
    diag_putc(hex[v & 0xF]);
}

static inline void vtoc_memset(void *s, int c, uint32_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
}

static inline int vtoc_memcmp(const void *s1, const void *s2, uint32_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++; p2++;
    }
    return 0;
}

/// @brief Read the Format-4 DSCB and extract the VTOC start address.
///
///        The Format-4 DSCB is always written by dasdload at cyl 0, head 1,
///        rec 1.  Its data field contains the VTOC pointer at bytes 1-5:
///          byte 1-2: VTOC start cylinder (big-endian)
///          byte 3-4: VTOC start head     (big-endian)
///          byte 5:   VTOC start record   (1-based)
///
///        We read data-only (96 bytes) since the key length may vary.
///        If the read fails or the format ID is not 0xF4, we fall back
///        to the compiled-in defaults.
///
/// @param schid      Subchannel ID
/// @param out_cyl    Receives VTOC start cylinder
/// @param out_head   Receives VTOC start head
static void vtoc_find_f4(uint32_t schid,
                         uint16_t *out_cyl, uint16_t *out_head) {
    *out_cyl  = VTOC_DEFAULT_CYL;
    *out_head = VTOC_DEFAULT_HEAD;

    static uint8_t f4_buf[96] __attribute__((aligned(4)));

    int rc = dasd_read_record(schid, 0, 1, 1,
                              CCW_CMD_READ_KD, f4_buf, 96U);
    if (rc != 0) {
        dasd_sense(schid);
        rc = dasd_read_record(schid, 0, 1, 1,
                              CCW_CMD_READ_DATA, f4_buf, 96U);
    }
    if (rc != 0) return; // Can't read F4 — use defaults

    if (f4_buf[0] != DSCB_FMT4_ID) return; // Not F4 — use defaults

    *out_cyl  = (uint16_t)((f4_buf[1] << 8) | f4_buf[2]);
    *out_head = (uint16_t)((f4_buf[3] << 8) | f4_buf[4]);

    print_msg("zxfl: vtoc f4 ptr cyl=");
    vtoc_print_hex8(*out_cyl);
    print_msg(" hd=");
    vtoc_print_hex8(*out_head);
    diag_putc('\n');
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
static int dscb_match(const uint8_t *buf, uint32_t buf_len,
                      const uint8_t *dsname_ebcdic,
                      dscb1_extent_t *out_extent) {
    uint32_t extent_off;

    if (buf_len >= 140U && buf[DSCB1_KD_FMTID_OFF] == DSCB_FMT1_ID) {
        extent_off = DSCB1_KD_EXTENT0_OFF;
    } else if (buf_len >= 96U && buf[DSCB1_D_FMTID_OFF] == DSCB_FMT1_ID) {
        extent_off = DSCB1_D_EXTENT0_OFF;
    } else {
        return 0; // F4, F5, empty, or unrecognised — not an error
    }

    if (vtoc_memcmp(buf, dsname_ebcdic, 44U) != 0) return 0;

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
    vtoc_memset(target, 0x40U, 44U);
    for (int i = 0; dsname[i] != '\0' && i < 44; i++)
        target[i] = ascii_to_ebcdic((uint8_t)dsname[i]);

    print_msg("zxfl: vtoc schid=");
    vtoc_print_hex8(schid);
    print_msg(" name=");
    for (int i = 0; i < 12; i++) vtoc_print_hex2(target[i]);
    diag_putc('\n');

    static uint8_t dscb_buf[140] __attribute__((aligned(4)));

    uint16_t cur_cyl  = VTOC_DEFAULT_CYL;
    uint16_t cur_head = VTOC_DEFAULT_HEAD;
    uint32_t consec   = 0;

    vtoc_find_f4(schid, &cur_cyl, &cur_head);

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

                    print_msg("zxfl: vtoc io err cyl=");
                    vtoc_print_hex8(cur_cyl);
                    print_msg(" hd=");
                    vtoc_print_hex8(cur_head);
                    print_msg(" rec=");
                    vtoc_print_hex2(rec);
                    diag_putc('\n');

                    if (consec >= VTOC_MAX_CONSECUTIVE_ERRORS)
                        return -1;
                    continue;
                }
            }

            uint8_t fmtid = (buf_len >= 45U) ? dscb_buf[44] : 0;
            print_msg("zxfl: vtoc rec c=");
            vtoc_print_hex8(cur_cyl);
            print_msg(" h=");
            vtoc_print_hex8(cur_head);
            print_msg(" r=");
            vtoc_print_hex2(rec);
            print_msg(" fmt=");
            vtoc_print_hex2(fmtid);
            print_msg(" name=");
            for (int i = 0; i < 12; i++) vtoc_print_hex2(dscb_buf[i]);
            diag_putc('\n');

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
