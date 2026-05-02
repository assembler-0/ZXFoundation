// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.c
//
// Stage 2 main logic. Runs fully in 64-bit mode.
// Responsibilities:
//   1. STFLE — detect CPU facilities
//   2. SCLP init + READ_INFO — detect installed RAM, build memory map
//   3. Control register setup (CR0, CR6)
//   4. Parmfile read (ETC.ZXFOUNDATION.PARM) from DASD
//   5. ELF64 kernel load (ET_EXEC; ET_DYN stub)
//   6. Fill zxfl_boot_protocol_t
//   7. Jump to kernel entry

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

#define ZX_NUCLEUS_NAME           "CORE.ZXFOUNDATION.NUCLEUS"
#define ZX_PARMFILE_NAME          "ETC.ZXFOUNDATION.PARM"
#define ZX_LOADER_PARMFILE_NAME   "ETC.ZXFOUNDATIONLOADER.PARM"

static zxfl_boot_protocol_t  s_proto;
static char                  s_cmdline[512] = {};
static char                  s_loader_cmdline[512] = {};

/// @brief Configure CR0 and CR6 for kernel entry.
///
///        CR0: clear the AFP-register control (bit 45) and the vector
///        facility control (bit 46) so the kernel can enable them itself.
///        Set the storage-protection override (bit 13) off — kernel manages
///        its own protection keys.
///        CR6: clear all I/O interrupt subclass masks so the kernel starts
///        with a clean interrupt state.
static void setup_control_regs(void) {
    uint64_t cr6 = 0;
    __asm__ volatile ("lctlg 6,6,%0" :: "Q" (cr6));
}

/// @brief Read the parmfile from DASD into s_cmdline.
///        If the dataset is not found or unreadable, s_cmdline is left empty.
/// @param schid Subchannel ID.
static void load_nucleus_parmfile(uint32_t schid) {
    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, ZX_PARMFILE_NAME, &ext) < 0) {
        print("zxfl01: etc.zxfoundation.parm not found");
        return;
    }

    static uint8_t parm_block[DASD_BLOCK_SIZE];
    if (dasd_read_record(schid, ext.begin_cyl, ext.begin_head, 1,
                         CCW_CMD_READ_DATA, parm_block, DASD_BLOCK_SIZE) < 0)
        return;

    uint32_t i = 0;
    while (i < sizeof(s_cmdline) - 1) {
        const uint8_t c = parm_block[i];
        if (c == 0 || c == '\n' || c == '\r')
            break;
        s_cmdline[i] = (char)c;
        i++;
    }
    s_cmdline[i] = '\0';
}

/// @brief Read the parmfile from DASD into s_cmdline.
///        If the dataset is not found or unreadable, s_cmdline is left empty.
/// @param schid Subchannel ID.
static void load_loader_parmfile(uint32_t schid) {
    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, ZX_LOADER_PARMFILE_NAME, &ext) < 0) {
        print("zxfl01: etc.zxfoundation.parm not found");
        return;
    }

    static uint8_t loader_parm_block[DASD_BLOCK_SIZE];
    if (dasd_read_record(schid, ext.begin_cyl, ext.begin_head, 1,
                         CCW_CMD_READ_DATA, loader_parm_block, DASD_BLOCK_SIZE) < 0)
        return;

    uint32_t i = 0;
    while (i < sizeof(s_loader_cmdline) - 1) {
        const uint8_t c = loader_parm_block[i];
        if (c == 0 || c == '\n' || c == '\r')
            break;
        s_loader_cmdline[i] = (char)c;
        i++;
    }
    s_loader_cmdline[i] = '\0';
}

/// @brief Transfer control to the kernel.
///        R2 = pointer to boot protocol (physical address).
///        R14 = kernel entry point.
/// @param proto  Pointer to the filled boot protocol.
/// @param entry  Kernel entry point (physical address for ET_EXEC,
///               load_bias + e_entry for ET_DYN).
[[noreturn]] static void jump_to_kernel(zxfl_boot_protocol_t *proto,
                                        uint64_t entry) {
    register uint64_t r2  __asm__("2")  = (uint64_t)proto;
    register uint64_t r14 __asm__("14") = entry;
    __asm__ volatile (
        "br     %[entry]\n"
        :
        : [entry] "r" (r14), "r" (r2)
        : "memory"
    );
    __builtin_unreachable();
}

/// @brief Stage 2 main entry point. Called from entry.S with R2 = schid.
/// @param schid Subchannel ID of the IPL device.
[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader 26h1 - core.zxfoundationloader01.sys initializing\n");

    s_proto.fac_count = stfle_detect(s_proto.stfle_fac);

    setup_control_regs();

    load_nucleus_parmfile(schid);
    s_proto.cmdline = s_cmdline;

    load_loader_parmfile(schid);

    dscb1_extent_t kernel_ext;
    if (dasd_find_dataset(schid, ZX_NUCLEUS_NAME, &kernel_ext) < 0)
        panic("zxfl01: core.zxfoundation.nucleus not found in vtoc\n");

    uint64_t entry_point = 0;
    uint32_t load_base   = 0;
    uint32_t load_size   = 0;

    if (zxfl_load_elf64(schid, &kernel_ext,
                        &entry_point, &load_base, &load_size) < 0)
        panic("zxfl01: core.zxfoundation.nucleus load error\n");

    s_proto.magic              = ZXFL_MAGIC;
    s_proto.version            = ZXFL_VERSION_2;
    s_proto.ipl_schid          = schid;
    s_proto.ipl_dev_type       = 0x3390U;
    s_proto.kernel_phys_start  = (uint64_t)load_base;
    s_proto.kernel_size        = (uint64_t)load_size;

    print("zxfl01: launching core.zxfoundation.nucleus\n");
    jump_to_kernel(&s_proto, entry_point);
}
