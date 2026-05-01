// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl.c
//
/// @brief ZXFL bootloader main entry point.
///        Orchestrates: VTOC search → ELF64 load → memory detection →
///        boot protocol assembly → kernel handoff.
///
///        All DASD I/O is in dasd.c; ELF parsing is in elfload.c;
///        console output is in diag.c.  This file contains only the
///        high-level sequencing and the CPU mode-switch trampoline.

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/dasd.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>

// ---------------------------------------------------------------------------
// Static boot-protocol storage
// (BSS — zeroed by the IPL entry before zxfl_main is called)
// ---------------------------------------------------------------------------

static zxfl_boot_protocol_t boot_protocol;
static zxfl_mmap_entry_t    memory_map[ZXFL_MMAP_MAX_ENTRIES];
static char                 cmdline_buf[256] = "console=ttyS0";

// ---------------------------------------------------------------------------
// Memory detection (implemented in zxfl_ipl.S)
// ---------------------------------------------------------------------------

extern uint32_t detect_memory_size(void);

// ---------------------------------------------------------------------------
// CPU mode-switch and kernel handoff
// ---------------------------------------------------------------------------

/// @brief Switch the CPU to 64-bit z/Arch mode and jump to the kernel.
///
///        Sequence:
///          1. SIGP Set Architecture (order 0x12, parm 1) → z/Arch mode.
///          2. SAM64 — switch addressing mode to 64-bit.
///          3. LCTLG 0-15 — load 64-bit control registers.
///             CR0 = 0: no special facilities enabled yet (kernel sets them).
///             All other CRs = 0: safe initial state.
///          4. LLGFR to zero-extend the 31-bit protocol pointer and entry
///             address into 64-bit registers.
///          5. BSM 0,R8 — branch to the kernel entry point.
///             BSM saves the current addressing mode into R0 bit 0 (ignored)
///             and branches to R8 in the mode indicated by R8 bit 0.
///             Since we just did SAM64, R8 bit 0 = 0 means 64-bit mode.
///
///        The control register table is embedded in the .text section
///        immediately after the asm block.  It must be 8-byte aligned.
[[noreturn]] void zxfl_jump_to_kernel(zxfl_boot_protocol_t *protocol,
                                      uint64_t entry) {
    register uint32_t r2 __asm__("2") = (uint32_t)(uintptr_t)protocol;
    register uint32_t r8 __asm__("8") = (uint32_t)entry;

    __asm__ volatile (
        // Switch to z/Arch mode.  R1 = 1 (z/Arch), SIGP order 0x12.
        ".machinemode zarch\n"
        "lhi    1, 1\n"
        "sigp   1, 0, 0x12\n"

        // Switch addressing mode to 64-bit.
        "sam64\n"

        // Load all 16 control registers from the embedded table.
        // CR0=0 disables all optional facilities; the kernel enables what
        // it needs after it has set up its own data structures.
        "larl   1, .Lcregs\n"
        "lctlg  0, 15, 0(1)\n"

        // Zero-extend the 31-bit pointers to 64-bit.
        "llgfr  2, 2\n"
        "llgfr  8, 8\n"

        // Branch to kernel entry.  BSM in 64-bit mode with R8 bit 0 = 0
        // branches in 64-bit mode.
        "bsm    0, 8\n"

        // Embedded control register table (16 × 8 bytes = 128 bytes).
        ".align 8\n"
        ".Lcregs:\n"
        ".quad 0\n"  // CR0:  no special facilities
        ".quad 0\n"  // CR1:  primary segment table (kernel sets this)
        ".quad 0\n"  // CR2:  access register translation
        ".quad 0\n"  // CR3:  (reserved)
        ".quad 0\n"  // CR4:  (reserved)
        ".quad 0\n"  // CR5:  (reserved)
        ".quad 0\n"  // CR6:  I/O interruption class mask
        ".quad 0\n"  // CR7:  secondary segment table
        ".quad 0\n"  // CR8:  access register 0 translation
        ".quad 0\n"  // CR9:  (reserved)
        ".quad 0\n"  // CR10: (reserved)
        ".quad 0\n"  // CR11: (reserved)
        ".quad 0\n"  // CR12: (reserved)
        ".quad 0\n"  // CR13: home segment table
        ".quad 0\n"  // CR14: machine check handling
        ".quad 0\n"  // CR15: linkage stack
        :
        : "r" (r2), "r" (r8)
        : "memory", "1"
    );

    // Unreachable — the BSM above transfers control to the kernel.
    // The while(1) satisfies [[noreturn]] for the compiler.
    while (1) { __asm__ volatile ("nop"); }
}

// ---------------------------------------------------------------------------
// Main bootloader entry
// ---------------------------------------------------------------------------

void zxfl_main(void) {
    // The IPL PSW stores the subchannel ID at lowcore offset 0xB8.
    uint32_t ipl_schid;
    __asm__ volatile ("l %0, 0xb8" : "=d" (ipl_schid));

    boot_protocol.magic      = ZXFL_MAGIC;
    boot_protocol.version    = ZXFL_VERSION_1;
    boot_protocol.ipl_schid  = ipl_schid;
    boot_protocol.ipl_dev_type = 0x3390U;

    print_msg("zxfl: ZXFoundationLoader 26h1 copyright (c) 2026 assembler-0 All rights reserved\n");

    // -----------------------------------------------------------------------
    // Step 1: Locate SYS1.NUCLEUS in the VTOC.
    // -----------------------------------------------------------------------
    dscb1_extent_t nucleus_ext;
    if (dasd_find_dataset(ipl_schid, "SYS1.NUCLEUS", &nucleus_ext) < 0) {
        // VTOC search failed — fall back to the track mandated by sysres.conf:
        // SYS1.NUCLEUS at TRK 15 = cyl 1, head 0 on a 3390-1 (15 heads/cyl).
        print_msg("zxfl: VTOC SYS1.NUCLEUS not found, using 1/0\n");
        nucleus_ext.begin_cyl  = 1U;
        nucleus_ext.begin_head = 0U;
        nucleus_ext.end_cyl    = 1U;
        nucleus_ext.end_head   = 14U; // Conservative: assume 1 cylinder
    } else {
        print_msg("zxfl: Found SYS1.NUCLEUS\n");
    }

    // -----------------------------------------------------------------------
    // Step 2: Load the ELF64 kernel.
    // -----------------------------------------------------------------------
    print_msg("zxfl: Loading kernel\n");

    uint64_t entry_point  = 0U;
    uint32_t load_base    = 0U;
    uint32_t load_size    = 0U;

    if (zxfl_load_elf64(ipl_schid, &nucleus_ext,
                        &entry_point, &load_base, &load_size) < 0) {
        print_msg("zxfl: Kernel load failed, aborting\n");
        return;
    }

    boot_protocol.kernel_start = (uint64_t)load_base;
    boot_protocol.kernel_size  = (uint64_t)load_size;
    boot_protocol.loader_start = 0U;
    // The loader occupies the first 64KB (lowcore + loader text/data/bss).
    boot_protocol.loader_size  = 0x10000U;

    // -----------------------------------------------------------------------
    // Step 3: Detect physical memory size.
    // -----------------------------------------------------------------------
    print_msg("zxfl: Detecting mem\n");
    uint64_t total_mem = (uint64_t)detect_memory_size();

    // -----------------------------------------------------------------------
    // Step 4: Build the memory map.
    // -----------------------------------------------------------------------
    // Region 0: Loader + lowcore (0x0000 – 0xFFFF)
    memory_map[0].start = 0U;
    memory_map[0].size  = 0x10000U;
    memory_map[0].type  = ZXFL_MEM_LOADER;

    // Region 1: Kernel image
    memory_map[1].start = (uint64_t)load_base;
    memory_map[1].size  = (uint64_t)load_size;
    memory_map[1].type  = ZXFL_MEM_KERNEL;

    // Region 2: Available memory above the kernel
    uint64_t avail_start = (uint64_t)load_base + (uint64_t)load_size;
    memory_map[2].start  = avail_start;
    memory_map[2].size   = (total_mem > avail_start)
                           ? (total_mem - avail_start)
                           : 0U;
    memory_map[2].type   = ZXFL_MEM_AVAILABLE;

    boot_protocol.mmap_count = 3U;
    boot_protocol.mmap       = memory_map;
    boot_protocol.cmdline    = cmdline_buf;

    // -----------------------------------------------------------------------
    // Step 5: Hand off to the kernel.
    // -----------------------------------------------------------------------
    print_msg("zxfl: Launching kernel\n");
    zxfl_jump_to_kernel(&boot_protocol, entry_point);
}
