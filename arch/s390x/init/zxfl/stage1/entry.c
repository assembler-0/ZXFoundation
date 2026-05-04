// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage1/entry.c

/// P.S.: could've done this in raw assembly but having some logs is nice

#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

#define STAGE2_LOAD_ADDR    0x20000UL
#define STAGE2_FILENAME     "CORE.ZXFOUNDATIONLOADER01.SYS"

/// @brief Jump to stage 2. Stage 2 is a flat binary linked at 0x20000,
///        so the entry point IS 0x20000. R2 carries schid per our ABI.
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
    print("zxfl00: ZXFoundationLoader (R) 26h1 CONFIDENTIAL - copyright (c) 2026 assembler-0 all rights reserved.\n");
    print("zxfl00: core.zxfoundationloader00.sys initializing\n");

    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, STAGE2_FILENAME, &ext) < 0)
        panic("zxfl00: core.zxfoundationloader01.sys not found\n");

    uint8_t *dst = (uint8_t *)STAGE2_LOAD_ADDR;
    uint16_t cyl  = ext.begin_cyl;
    uint16_t head = ext.begin_head;
    uint8_t  rec  = 1;

    while (cyl < ext.end_cyl ||
           (cyl == ext.end_cyl && head <= ext.end_head)) {
        int rc = dasd_read_next(schid, &cyl, &head, &rec,
                                CCW_CMD_READ_DATA, dst, DASD_BLOCK_SIZE);
        if (rc < 0)
            break;
        dst += DASD_BLOCK_SIZE;
    }

    if (*(volatile uint64_t *)STAGE2_LOAD_ADDR == 0)
        panic("zxfl00: core.zxfoundationloader01.sys load empty\n");

    print("zxfl00: launching core.zxfoundationloader00.sys\n");
    jump_to_stage2((uint64_t)schid);
}
