// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl.c
//
/// @brief ZXFL bootloader main entry point.
///
///        This is a dedicated single-volume boot loader.  The disk layout
///        is fixed by sysres.conf and never changes:
///
///          cyl 0, head 0          IPL track  (loader binary)
///          cyl 0, head 1, rec 1+  sys.zxfoundation.nucleus (ELF64 kernel, FB 4096)
///
///        There is no VTOC.  The kernel location is derived directly from
///        the sysres.conf allocation ("TRK 15" = track 15 on a 3390-1 with
///        15 heads/cyl → cyl 1, head 0).  We do not scan a VTOC because
///        dasdload does not write one when no VTOC statement is present.
///
///        Sequence: load ELF64 kernel → build boot protocol → hand off.

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>

// ---------------------------------------------------------------------------
// Kernel location on disk
//
// sysres.conf: "sys.zxfoundation.nucleus SEQ zxfoundation.krnl TRK 15 0 0 PS FB 4096 4096 0"
//
// dasdload allocates datasets sequentially.  With no VTOC statement and
// the kernel as the only dataset, dasdload places sys.zxfoundation.nucleus immediately
// after the IPL track:
//   Track  0 = cyl 0, head 0  → IPL track (loader)
//   Track  1 = cyl 0, head 1  → sys.zxfoundation.nucleus begins here
//
// The "TRK 15" primary allocation means 15 tracks are reserved, but the
// dataset starts at the first available track after the IPL track.
// Records are 4096-byte fixed-block with no keys (kl=0, dl=4096).
// ---------------------------------------------------------------------------
#define KERNEL_START_CYL    0U
#define KERNEL_START_HEAD   1U

// ---------------------------------------------------------------------------
// Static boot-protocol storage (BSS — zeroed before zxfl_main is called)
// ---------------------------------------------------------------------------
static zxfl_boot_protocol_t boot_protocol;
static zxfl_mmap_entry_t    memory_map[ZXFL_MMAP_MAX_ENTRIES];
static char                 cmdline_buf[256] = "console=ttyS0";

// Memory hint passed to the kernel (hercules.cnf MAINSIZE 512).
// The kernel performs authoritative detection after switching to z/Arch.
#define ZXFL_MEM_HINT_BYTES  (512U * 1024U * 1024U)

// ---------------------------------------------------------------------------
// CPU mode-switch and kernel handoff
// ---------------------------------------------------------------------------

/// @brief Switch to 64-bit z/Arch mode and jump to the kernel entry point.
///
///        Steps:
///          1. SIGP Set Architecture (order 0x12, parm 1) → z/Arch mode.
///          2. SAM64 — switch addressing mode to 64-bit.
///          3. LCTLG 0-15 — load 64-bit control registers (all zero;
///             the kernel configures them after it starts).
///          4. LLGFR — zero-extend 31-bit pointers to 64-bit.
///          5. BSM 0,R8 — branch to the kernel in 64-bit mode.
[[noreturn]] void zxfl_jump_to_kernel(zxfl_boot_protocol_t *protocol,
                                      uint64_t entry) {
    register uint32_t r2 __asm__("2") = (uint32_t)(uintptr_t)protocol;
    register uint32_t r8 __asm__("8") = (uint32_t)entry;

    __asm__ volatile (
        ".machinemode zarch\n"
        "lhi    1, 1\n"
        "sigp   1, 0, 0x12\n"   // SIGP Set Architecture → z/Arch
        "sam64\n"                // Switch to 64-bit addressing
        "larl   1, .Lcregs\n"
        "lctlg  0, 15, 0(1)\n"  // Load all CRs from embedded table
        "llgfr  2, 2\n"         // Zero-extend protocol pointer
        "llgfr  8, 8\n"         // Zero-extend entry address
        "bsm    0, 8\n"         // Branch to kernel in 64-bit mode
        ".align 8\n"
        ".Lcregs:\n"
        ".quad 0\n" ".quad 0\n" ".quad 0\n" ".quad 0\n"
        ".quad 0\n" ".quad 0\n" ".quad 0\n" ".quad 0\n"
        ".quad 0\n" ".quad 0\n" ".quad 0\n" ".quad 0\n"
        ".quad 0\n" ".quad 0\n" ".quad 0\n" ".quad 0\n"
        :
        : "r" (r2), "r" (r8)
        : "memory", "1"
    );
    while (1) { __asm__ volatile ("nop"); }
}

// ---------------------------------------------------------------------------
// Main bootloader entry
// ---------------------------------------------------------------------------

void zxfl_main(void) {
    // Read the IPL subchannel ID from lowcore 0xB8.
    uint32_t ipl_schid;
    __asm__ volatile ("l %0, 0xb8" : "=d" (ipl_schid));

    boot_protocol.magic        = ZXFL_MAGIC;
    boot_protocol.version      = ZXFL_VERSION_1;
    boot_protocol.ipl_schid    = ipl_schid;
    boot_protocol.ipl_dev_type = 0x3390U;

    print_msg("zxfl: ZXFoundationLoader 26h1 copyright (c) 2026 assembler-0 All rights reserved\n");

    print_msg("zxfl: Loading sys.zxfoundation.nucleus\n");
    dscb1_extent_t kernel_ext = {
        .begin_cyl  = KERNEL_START_CYL,
        .begin_head = KERNEL_START_HEAD,
        .end_cyl    = 1U,
        .end_head   = 14U,
    };

    uint64_t entry_point = 0U;
    uint32_t load_base   = 0U;
    uint32_t load_size   = 0U;

    if (zxfl_load_elf64(ipl_schid, &kernel_ext,
                        &entry_point, &load_base, &load_size) < 0) {
        print_msg("zxfl: load failed, aborting\n");
        return;
    }

    boot_protocol.kernel_start = (uint64_t)load_base;
    boot_protocol.kernel_size  = (uint64_t)load_size;
    boot_protocol.loader_start = 0U;
    boot_protocol.loader_size  = 0x10000U;  // First 64 KB = loader footprint

    // -----------------------------------------------------------------------
    // Step 2: Build the memory map.
    // -----------------------------------------------------------------------
    uint64_t total_mem   = (uint64_t)ZXFL_MEM_HINT_BYTES;
    uint64_t avail_start = (uint64_t)load_base + (uint64_t)load_size;

    memory_map[0] = (zxfl_mmap_entry_t){
        .start = 0U,
        .size  = 0x10000U,
        .type  = ZXFL_MEM_LOADER,
        ._pad  = 0U,
    };
    memory_map[1] = (zxfl_mmap_entry_t){
        .start = (uint64_t)load_base,
        .size  = (uint64_t)load_size,
        .type  = ZXFL_MEM_KERNEL,
        ._pad  = 0U,
    };
    memory_map[2] = (zxfl_mmap_entry_t){
        .start = avail_start,
        .size  = (total_mem > avail_start) ? (total_mem - avail_start) : 0U,
        .type  = ZXFL_MEM_AVAILABLE,
        ._pad  = 0U,
    };

    boot_protocol.mmap_count = 3U;
    boot_protocol._pad       = 0U;
    boot_protocol.mmap       = memory_map;
    boot_protocol.cmdline    = cmdline_buf;

    print_msg("zxfl: Launching sys.zxfoundation.nucleus\n");
    zxfl_jump_to_kernel(&boot_protocol, entry_point);
}
