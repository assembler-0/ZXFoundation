// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/init/zxfl/stage1/entry.c

/// P.S.: could've done this in raw assembly but having some logs is nice

#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <zxfoundation/zxconfig.h>

#define STAGE2_LOAD_ADDR    0x20000UL
#define STAGE2_FILENAME     "CORE.ZXFOUNDATIONLOADER01.SYS"

/// @brief Jump to stage 2. Stage 2 is a flat binary linked at 0x20000,
/// @param schid Subchannel ID of the IPL device.
[[noreturn]] static void jump_to_stage2(uint64_t schid) {
    register uint64_t r2  __asm__("2")  = schid;
    register uint64_t r14 __asm__("14") = STAGE2_LOAD_ADDR;
    __asm__ volatile (
        "br     %[entry]\n"
        :
        : [entry] "r" (r14), "r" (r2)
        : "memory"
    );
    __builtin_unreachable();
}

/// @brief Stage 1 entry point. Called from head.S in 64-bit mode.
/// @param schid Subchannel ID from lowcore 0xB8.
[[noreturn]] void zxfl00_entry(uint32_t schid) {
    diag_setup();
    print("zxfl00: ZXFoundationLoader (TM) " CONFIG_ZX_RELEASE " - Copyright (c) " CONFIG_ZX_COPYRIGHT_DATE " assembler-0. Licensed under the Apache License, Version 2.0.\n");
    print("zxfl00: core.zxfoundationloader00.sys initializing\n");

    dscb1_extent_t ext;
    dasd_dataset_t ds;
    if (dasd_find_dataset_extents(schid, STAGE2_FILENAME, &ds) < 0 ||
        ds.count == 0)
        panic("zxfl00: core.zxfoundationloader01.sys not found\n");
    ext = ds.extents[0];

    uint8_t *dst = (uint8_t *)STAGE2_LOAD_ADDR;
    uint16_t cyl  = ext.begin_cyl;
    uint16_t head = ext.begin_head;
    uint8_t  rec  = 1;
    uint32_t loaded = 0;

    const uint32_t base_trk =
        (uint32_t)ext.begin_cyl * DASD_3390_HEADS_PER_CYL + ext.begin_head;

    while (1) {
        if (ds.lstar_valid) {
            const uint32_t abs_trk =
                (uint32_t)cyl * DASD_3390_HEADS_PER_CYL + head;
            const uint32_t rel_trk = abs_trk - base_trk;
            if (rel_trk == ds.lstar_track && rec > ds.lstar_rec)
                break; /* DS1LSTAR indicates EOF */
        } else if (cyl > ext.end_cyl ||
                   (cyl == ext.end_cyl && head > ext.end_head)) {
            break;
        }

        int rc = dasd_read_next(schid, &cyl, &head, &rec,
                                CCW_CMD_READ_DATA, dst, DASD_BLOCK_SIZE, 12, 15);
        if (rc < 0) {
            if (ds.lstar_valid) {
                const uint32_t abs_trk =
                    (uint32_t)cyl * DASD_3390_HEADS_PER_CYL + head;
                const uint32_t rel_trk = abs_trk - base_trk;
                const bool at_end =
                    rel_trk == ds.lstar_track && rec == ds.lstar_rec;
                if (!at_end)
                    panic("zxfl00: core.zxfoundationloader01.sys read failure - load truncated\n");
            }
            break;
        }
        dst += DASD_BLOCK_SIZE;
        loaded += DASD_BLOCK_SIZE;
    }

    if (loaded == 0)
        panic("zxfl00: core.zxfoundationloader01.sys load empty\n");

    const volatile uint64_t w0 = *(const volatile uint64_t *)STAGE2_LOAD_ADDR;
    if (w0 == 0 || w0 == 0x0000800180000000ULL)
        panic("zxfl00: core.zxfoundationloader01.sys load corrupt\n");

    print("zxfl00: launching core.zxfoundationloader01.sys\n");
    jump_to_stage2((uint64_t)schid);
}
