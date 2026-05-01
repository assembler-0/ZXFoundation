// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage1/stage1_main.c

#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

#define STAGE2_LOAD_ADDR    0x20000U
#define STAGE2_FILENAME     "CORE.ZXFOUNDATIONLOADER01.SYS"

/// @brief Jump to Stage 2 in 64-bit mode.
/// @param schid Subchannel ID.
/// @param entry Entry point address.
[[noreturn]] static void jump_to_stage2(uint32_t schid, uint32_t entry) {
    register uint32_t r2 __asm__("2") = schid;
    register uint32_t r3 __asm__("3") = entry;

    __asm__ volatile (
        ".machinemode zarch\n"
        "lhi    1, 1\n"
        "sigp   1, 0, 0x12\n"   /* Set Architecture: z/Arch mode */
        "sam64\n"               /* Switch to 64-bit addressing */
        "llgfr  2, 2\n"
        "llgfr  3, 3\n"
        "br     3\n"
        :
        : "r" (r2), "r" (r3)
        : "memory", "1"
    );
    for (;;) { __asm__ volatile("nop"); }
}

/// @brief Stage 1 entry point (31-bit).
/// @param ipl_schid Subchannel ID of the device we booted from.
void zxfl00_entry(const uint32_t ipl_schid) {
    dscb1_extent_t extent;
    uint8_t *load_ptr = (uint8_t *)STAGE2_LOAD_ADDR;
    diag_setup();

    print_msg("zxfl: ZXFoundationLoader 26h1 - core.zxfoundationloader00.sys initializing\n");

    if (dasd_find_dataset(ipl_schid, STAGE2_FILENAME, &extent) < 0) {
        panic("zxfl: core.zxfoundationloader01.sys loader not found on DASD\n");
    }

    uint16_t cur_cyl  = extent.begin_cyl;
    uint16_t cur_head = extent.begin_head;

    for (uint32_t i = 0; i < 4096; i++) load_ptr[i] = 0xAA; // poison

    while (cur_cyl < extent.end_cyl || (cur_cyl == extent.end_cyl && cur_head <= extent.end_head)) {
        for (uint8_t r = 1; r <= 12; r++) {
            int rc = dasd_read_record(ipl_schid, cur_cyl, cur_head, r, CCW_CMD_READ_DATA, load_ptr, 4096);
            if (rc == 0) {
                load_ptr += 4096;
            } else {
                break; 
            }
        }
        cur_head++;
        if (cur_head >= DASD_3390_HEADS_PER_CYL) {
            cur_head = 0;
            cur_cyl++;
        }
    }

    uint32_t magic = *(uint32_t*)STAGE2_LOAD_ADDR;

    if (magic == 0xAAAAAAAA || magic == 0) {
        print_msg("zxfl: core.zxfoundationloader01.sys not loaded correctly\n");
        return;
    }

    print_msg("zxfl: launching core.zxfoundation01.sys\n");
    jump_to_stage2(ipl_schid, STAGE2_LOAD_ADDR);
}
