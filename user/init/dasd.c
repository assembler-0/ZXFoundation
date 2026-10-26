/// SPDX-License-Identifier: Apache-2.0
/// @file user/init/dasd.c

#include <sys/inc64/nuclbase.h>

typedef struct {
    nb_u64 request_id;
    nb_u32 ccw_offset;
    nb_u16 ccw_count;
    nb_u8  flags;
    nb_u8  _reserved;
} dasd_io_request;

typedef struct {
    nb_u64 request_id;
    nb_u32 residual_count;
    nb_u8  device_status;
    nb_u8  subchannel_status;
    nb_u16 flags;
} dasd_io_completion;

#define DCCW_CMD_SEEK          0x07U
#define DCCW_CMD_SEARCH_ID_EQ  0x31U
#define DCCW_CMD_TIC           0x08U
#define DCCW_CMD_READ_DATA     0x06U
#define DCCW_CMD_READ_KD       0x0EU
#define DCCW_FLAG_CC           0x40U
#define DCCW_FLAG_SLI          0x20U

#define DASD_3390_HEADS        15U
#define ECKD_3390_RECS_PER_TRK 12U
#define DASD_BLOCK_SIZE        4096U

#define VTOC_DEFAULT_CYL       0U
#define VTOC_DEFAULT_HEAD      1U
#define VTOC_MAX_TRACKS        30U
#define VTOC_MAX_RECORDS       20U
#define VTOC_MAX_CONSEC        3U
#define VTOC_MAX_EXTENTS       16U

#define DSCB1_KD_FMTID_OFF     44U
#define DSCB1_KD_FLAG1_OFF     61U
#define DS1LARGE_FLAG          0x08U
#define DSCB1_KD_LSTAR_OFF     98U
#define DSCB1_KD_TTTHI_OFF     104U
#define DSCB1_KD_EXTENT0_OFF   105U
#define DSCB1_KD_PTR3_OFF      135U
#define DSCB3_KD_FMTID_OFF     5U
#define DSCB3_KD_EXTENT0_OFF   6U
#define DSCB3_KD_PTR_OFF       136U
#define DSCB3_KD_EXTENTS       13U
#define DSCB_FMT1_ID           0xF1U
#define DSCB_FMT3_ID           0xF3U

static nb_u64 drv_token;
static volatile nb_u8 *drv_win;        // mapped window base
static nb_u64 drv_req_id;

typedef struct {
    nb_u16 begin_cyl;
    nb_u16 begin_head;
    nb_u16 end_cyl;
    nb_u16 end_head;
} dasd_extent;

typedef struct {
    nb_u32 count;
    dasd_extent extents[VTOC_MAX_EXTENTS];
    nb_u32 lstar_track;
    nb_u8  lstar_rec;
    nb_u8  lstar_valid;
} dasd_dataset;

// CCW1: cmd(1) flags(1) count(2) cda(4), big-endian fields.
static void ccw_set(volatile nb_u8 *c, nb_u8 cmd, nb_u8 flags, nb_u16 count,
                    nb_u32 rel_addr) {
    c[0] = cmd;
    c[1] = flags;
    c[2] = (nb_u8)(count >> 8);
    c[3] = (nb_u8)count;
    c[4] = (nb_u8)(rel_addr >> 24);
    c[5] = (nb_u8)(rel_addr >> 16);
    c[6] = (nb_u8)(rel_addr >> 8);
    c[7] = (nb_u8)rel_addr;
}

static void arg_seek(volatile nb_u8 *p, nb_u16 cyl, nb_u16 head) {
    p[0] = 0; p[1] = 0;                                   // reserved
    p[2] = (nb_u8)(cyl >> 8); p[3] = (nb_u8)cyl;
    p[4] = (nb_u8)(head >> 8); p[5] = (nb_u8)head;
}

static void arg_search(volatile nb_u8 *p, nb_u16 cyl, nb_u16 head, nb_u8 rec) {
    p[0] = (nb_u8)(cyl >> 8); p[1] = (nb_u8)cyl;
    p[2] = (nb_u8)(head >> 8); p[3] = (nb_u8)head;
    p[4] = rec;
}

/// @brief Basic Sense (0x04) — clears a latched unit check/exception.
static void dasd_sense(void) {
    volatile nb_u8 *chain = drv_win + 0x0;
    ccw_set(chain, 0x04U /* SENSE */, DCCW_FLAG_SLI, 32, 0x400);
    dasd_io_request req;
    req.request_id = ++drv_req_id;
    req.ccw_offset = 0;
    req.ccw_count = 1;
    req.flags = 0;
    req._reserved = 0;
    if (nb_io_submit(drv_token, (nb_u64)&req, 0, 0, 0) == NB_ERR)
        return;
    for (nb_u64 spins = 0; spins < 2000000ULL; spins++) {
        dasd_io_completion comp;
        if (nb_io_completion_dequeue(drv_token, (nb_u64)&comp, 0, 0, 0)
                == NB_ERR)
            continue;
        if (comp.request_id != req.request_id)
            continue;
        return;
    }
}

/// @brief Submit a 4-CCW SEEK/SEARCH/TIC/READ chain and await completion.
static int dasd_read_record(nb_u16 cyl, nb_u16 head, nb_u8 rec,
                            nb_u8 rd_cmd, nb_u32 len) {
    volatile nb_u8 *chain = drv_win + 0x0;
    volatile nb_u8 *seek  = drv_win + 0x100;
    volatile nb_u8 *search = drv_win + 0x120;

    arg_seek(seek, cyl, head);
    arg_search(search, cyl, head, rec);

    ccw_set(chain + 0,  DCCW_CMD_SEEK, DCCW_FLAG_CC | DCCW_FLAG_SLI,
            6, 0x100);
    ccw_set(chain + 8,  DCCW_CMD_SEARCH_ID_EQ, DCCW_FLAG_CC | DCCW_FLAG_SLI,
            5, 0x120);
    ccw_set(chain + 16, DCCW_CMD_TIC, 0, 0, 8);
    ccw_set(chain + 24, rd_cmd, DCCW_FLAG_SLI,
            (nb_u16)len, 0x200);

    dasd_io_request req;
    req.request_id = ++drv_req_id;
    req.ccw_offset = 0;
    req.ccw_count = 4;
    req.flags = 0;
    req._reserved = 0;

    if (nb_io_submit(drv_token, (nb_u64)&req, 0, 0, 0) == NB_ERR)
        return -1;

    for (nb_u64 spins = 0; spins < 2000000ULL; spins++) {
        dasd_io_completion comp;
        if (nb_io_completion_dequeue(drv_token, (nb_u64)&comp, 0, 0, 0)
                == NB_ERR)
            continue;                       // not ready — poll again
        if (comp.request_id != req.request_id)
            continue;
        if (comp.subchannel_status != 0) {
            dasd_sense();
            return -1;
        }
        if ((comp.device_status & 0x03U) != 0U) {
            dasd_sense();                  // clear the latched UC/UE
            return -1;
        }
        if ((comp.device_status & 0x0CU) != 0x0CU)
            continue;                      // not terminal yet — keep polling
        return 0;
    }
    return -1;
}

/// @brief Read one dataset block (LBA) into the window transfer buffer.
static int dasd_read_block(nb_u32 lba) {
    const nb_u32 recs_per_cyl = DASD_3390_HEADS * ECKD_3390_RECS_PER_TRK;
    const nb_u32 cyl = lba / recs_per_cyl;
    const nb_u32 rem = lba % recs_per_cyl;
    return dasd_read_record((nb_u16)cyl, (nb_u16)(rem / ECKD_3390_RECS_PER_TRK),
                            (nb_u8)((rem % ECKD_3390_RECS_PER_TRK) + 1U),
                            DCCW_CMD_READ_DATA, DASD_BLOCK_SIZE);
}

static const nb_u8 *dasd_xfer_buf(void) {
    return drv_win + 0x200;
}

static nb_u8 ascii_to_ebcdic(nb_u8 a) {
    if (a >= 'A' && a <= 'I') return (nb_u8)(0xC1 + (a - 'A'));
    if (a >= 'J' && a <= 'R') return (nb_u8)(0xD1 + (a - 'J'));
    if (a >= 'S' && a <= 'Z') return (nb_u8)(0xE2 + (a - 'S'));
    if (a >= '0' && a <= '9') return (nb_u8)(0xF0 + (a - '0'));
    if (a == '.') return 0x4B;
    return 0x40;
}

static int parse_extent(const nb_u8 *e, dasd_extent *out) {
    if (e[0] == 0x00U) return 0;
    out->begin_cyl  = (nb_u16)((e[2] << 8) | e[3]);
    out->begin_head = (nb_u16)((e[4] << 8) | e[5]);
    out->end_cyl    = (nb_u16)((e[6] << 8) | e[7]);
    out->end_head   = (nb_u16)((e[8] << 8) | e[9]);
    return 1;
}

/// @brief Read a DSCB (key+data first, data-only fallback) into the window.
static nb_u32 read_dscb(nb_u16 cyl, nb_u16 head, nb_u8 rec) {
    if (dasd_read_record(cyl, head, rec, DCCW_CMD_READ_KD, 140) == 0)
        return 140;
    if (dasd_read_record(cyl, head, rec, DCCW_CMD_READ_DATA, 96) == 0)
        return 96;
    return 0;
}

static void collect_dscb3(nb_u16 cyl, nb_u16 head, nb_u8 rec,
                          dasd_dataset *ds) {
    for (nb_u32 depth = 0; depth < 8 && ds->count < VTOC_MAX_EXTENTS; depth++) {
        if (cyl == 0 && head == 0 && rec == 0) break;
        nb_u32 len = read_dscb(cyl, head, rec);
        if (len != 140) break;
        const nb_u8 *buf = dasd_xfer_buf();
        if (buf[DSCB3_KD_FMTID_OFF] != DSCB_FMT3_ID) break;
        for (nb_u32 i = 0; i < DSCB3_KD_EXTENTS
                && ds->count < VTOC_MAX_EXTENTS; i++) {
            dasd_extent ext;
            if (parse_extent(buf + DSCB3_KD_EXTENT0_OFF + i * 10U, &ext))
                ds->extents[ds->count++] = ext;
        }
        const nb_u8 *ptr = buf + DSCB3_KD_PTR_OFF;
        cyl  = (nb_u16)((ptr[0] << 8) | ptr[1]);
        head = (nb_u16)((ptr[2] << 8) | ptr[3]);
        rec  = ptr[4];
    }
}

int dasd_find_dataset(const char *dsname, dasd_dataset *out_ds) {
    nb_u8 target[44];
    for (int i = 0; i < 44; i++) target[i] = 0x40;
    for (int i = 0; dsname[i] != '\0' && i < 44; i++)
        target[i] = ascii_to_ebcdic((nb_u8)dsname[i]);

    nb_u16 cur_cyl = VTOC_DEFAULT_CYL;
    nb_u16 cur_head = VTOC_DEFAULT_HEAD;
    nb_u32 consec = 0;

    for (nb_u32 track = 0; track < VTOC_MAX_TRACKS; track++) {
        for (nb_u8 rec = 1; rec <= VTOC_MAX_RECORDS; rec++) {
            nb_u32 len = read_dscb(cur_cyl, cur_head, rec);
            if (len != 140) {
                if (++consec >= VTOC_MAX_CONSEC) return -1;
                continue;
            }
            consec = 0;
            const nb_u8 *buf = dasd_xfer_buf();
            if (buf[DSCB1_KD_FMTID_OFF] != DSCB_FMT1_ID) continue;

            for (int i = 0; i < 44; i++)
                if (buf[i] != target[i]) goto next_rec;

            // Match: last-used TTR + extents + DSCB3 chain.
            {
                const nb_u8 *ls = buf + DSCB1_KD_LSTAR_OFF;
                const int larg = (buf[DSCB1_KD_FLAG1_OFF] & DS1LARGE_FLAG) != 0;
                out_ds->lstar_track = (nb_u32)((ls[0] << 8) | ls[1])
                    | (larg ? ((nb_u32)buf[DSCB1_KD_TTTHI_OFF] << 16) : 0U);
                out_ds->lstar_rec = ls[2];
                out_ds->lstar_valid = (ls[0] | ls[1] | ls[2]) != 0;
            }
            out_ds->count = 0;
            for (nb_u32 i = 0; i < 3 && out_ds->count < VTOC_MAX_EXTENTS; i++) {
                dasd_extent ext;
                if (parse_extent(buf + DSCB1_KD_EXTENT0_OFF + i * 10U, &ext))
                    out_ds->extents[out_ds->count++] = ext;
            }
            {
                const nb_u8 *ptr = buf + DSCB1_KD_PTR3_OFF;
                nb_u16 p_cyl = (nb_u16)((ptr[0] << 8) | ptr[1]);
                nb_u16 p_head = (nb_u16)((ptr[2] << 8) | ptr[3]);
                nb_u8 p_rec = ptr[4];
                if (p_cyl | p_head | p_rec)
                    collect_dscb3(p_cyl, p_head, p_rec, out_ds);
            }
            return (out_ds->count > 0) ? 0 : -1;
next_rec:;
        }
        cur_head++;
        if (cur_head >= DASD_3390_HEADS) {
            cur_head = 0;
            cur_cyl++;
        }
        consec = 0;
    }
    return -1;
}

/// @brief Read a dataset sequentially into dst (block-granular).
/// @return Blocks read, or -1.
int dasd_read_dataset(const char *dsname, nb_u8 *dst, nb_u32 max_blocks) {
    dasd_dataset ds;
    if (dasd_find_dataset(dsname, &ds) < 0) return -1;

    const dasd_extent *e = &ds.extents[0];
    const nb_u32 recs_per_cyl = DASD_3390_HEADS * ECKD_3390_RECS_PER_TRK;
    nb_u32 blocks = 0;

    for (nb_u32 ec = e->begin_cyl; ec <= e->end_cyl && blocks < max_blocks; ec++) {
        const nb_u16 h_end = (ec == e->end_cyl) ? e->end_head : (DASD_3390_HEADS - 1);
        for (nb_u32 h = (ec == e->begin_cyl ? e->begin_head : 0);
             h <= h_end && blocks < max_blocks; h++) {
            for (nb_u32 r = 0; r < ECKD_3390_RECS_PER_TRK && blocks < max_blocks; r++) {
                const nb_u32 lba = ec * recs_per_cyl + h * ECKD_3390_RECS_PER_TRK + r;
                (void)lba;
                const int rr = dasd_read_record((nb_u16)ec, (nb_u16)h,
                                               (nb_u8)(r + 1U),
                                               DCCW_CMD_READ_DATA,
                                               DASD_BLOCK_SIZE);
                if (rr < 0)
                    return (blocks == 0) ? -1 : (int)blocks;
                for (nb_u32 i = 0; i < DASD_BLOCK_SIZE; i++)
                    dst[blocks * DASD_BLOCK_SIZE + i] = dasd_xfer_buf()[i];
                blocks++;
                if (ds.lstar_valid && blocks >= 64)
                    return (int)blocks;    // safety cap for v1
            }
        }
    }
    return (int)blocks;
}

/// @brief Bind the DASD and map the DMA window — the driver's boot.
/// @return 0 on success.
int dasd_init(nb_u64 dev_no) {
    nb_u64 tok = nb_io_bind(dev_no, 0, 0, 0, 0);
    if (tok == NB_ERR) return -1;
    drv_token = tok;

    nb_u64 ext = nb_io_dma_extent(tok, 0, 0, 0, 0);
    if (ext == NB_ERR) return -1;

    if (nb_mem_map(ext, 0x30000000ull, 0, 12288, 3) == NB_ERR) return -1;
    drv_win = (volatile nb_u8 *)0x30000000ull;
    return 0;
}
