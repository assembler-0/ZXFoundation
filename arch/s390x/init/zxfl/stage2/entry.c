// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/stage2_main.c

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

static zxfl_boot_protocol_t boot_protocol;
static zxfl_mmap_entry_t    memory_map[ZXFL_MMAP_MAX_ENTRIES];
static char                 cmdline_buf[256] = "console=ttyS0 root=/dev/dasda1";

#define ZXFL_MEM_HINT_BYTES  (1024ULL * 1024ULL * 1024ULL)
#define ZX_KERNEL_NAME       "CORE.ZXFOUNDATION.NUCLEUS"

/// @brief Final jump to kernel (64-bit).
/// @param protocol Pointer to boot protocol.
/// @param entry Kernel entry point.
[[noreturn]] static void jump_to_kernel(zxfl_boot_protocol_t *protocol, uint64_t entry) {
    register uint64_t r2 __asm__("2") = (uint64_t)protocol;
    register uint64_t r8 __asm__("8") = entry;

    __asm__ volatile (
        "br     8\n"
        :
        : "r" (r2), "r" (r8)
        : "memory"
    );
    for (;;) { __asm__ volatile("nop"); }
}

/// @brief Stage 2 main logic (64-bit).
/// @param schid Subchannel ID inherited from Stage 1.
void zxfl01_entry(uint32_t schid) {
    print_msg("zxfl: ZXFoundationLoader 26h1 - core.zxfoundationloader01.sys initializing\n");

    boot_protocol.magic        = ZXFL_MAGIC;
    boot_protocol.version      = ZXFL_VERSION_1;
    boot_protocol.ipl_schid    = schid;
    boot_protocol.ipl_dev_type = 0x3390U;

    dscb1_extent_t kernel_ext;
    if (dasd_find_dataset(schid, ZX_KERNEL_NAME, &kernel_ext) < 0) {
        panic("core.zxfoundation.nucleus not found in VTOC\n");
    }
    print_msg("zxfl: found core.zxfoundation.nucleus\n");

    uint64_t entry_point = 0;
    uint32_t load_base   = 0;
    uint32_t load_size   = 0;

    print_msg("zxfl: loading core.zxfoundation.nucleus\n");
    if (zxfl_load_elf64(schid, &kernel_ext, &entry_point, &load_base, &load_size) < 0) {
        print_msg("core.zxfoundation.nucleus load failed\n");
        return;
    }

    boot_protocol.kernel_start = (uint64_t)load_base;
    boot_protocol.kernel_size  = (uint64_t)load_size;
    boot_protocol.loader_start = 0U;
    boot_protocol.loader_size  = 0x30000U; // Stage 1 + Stage 2 memory footprint

    // Setup initial memory map
    memory_map[0] = (zxfl_mmap_entry_t){ .start = 0, .size = 0x30000, .type = ZXFL_MEM_LOADER };
    memory_map[1] = (zxfl_mmap_entry_t){ .start = load_base, .size = load_size, .type = ZXFL_MEM_KERNEL };
    memory_map[2] = (zxfl_mmap_entry_t){ 
        .start = (uint64_t)load_base + load_size,
        .size  = ZXFL_MEM_HINT_BYTES - (load_base + load_size),
        .type  = ZXFL_MEM_AVAILABLE
    };

    boot_protocol.mmap_count = 3;
    boot_protocol.mmap       = memory_map;
    boot_protocol.cmdline    = cmdline_buf;

    print_msg("zxfl: launching core.zxfoundation.nucleus\n");
    jump_to_kernel(&boot_protocol, entry_point);
}
