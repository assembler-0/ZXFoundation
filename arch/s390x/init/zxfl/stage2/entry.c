// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.c
//
// Stage 2 main logic.  Runs fully in 64-bit z/Architecture mode.
//
// Boot sequence:
//   1.  Lowcore setup — install safe disabled-wait new PSWs.
//   2.  STFLE — detect all CPU facilities (up to 32 dwords).
//   3.  SMP enumeration — find all CPUs via STSI; stop APs.
//   4.  Control register setup — CR0, CR6 for kernel entry state.
//   5.  Parmfile read — ETC.ZXFOUNDATION.PARM from DASD.
//   6.  syssize= parse — determine memory probe ceiling from parmfile.
//   7.  Memory probe — walk physical memory in 1 MB frames to build map.
//   8.  ELF64 kernel load — CORE.ZXFOUNDATION.NUCLEUS from DASD.
//   9.  Protocol fill — populate zxfl_boot_protocol_t v3.
//  10.  Binding token — compute ZXFL_SEED ^ stfle_fac[0] ^ schid.
//  11.  Jump to kernel — R2 = &proto, R14 = entry_point.

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/lowcore.h>
#include <arch/s390x/init/zxfl/smp.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <arch/s390x/init/zxfl/parmfile.h>

#define ZX_NUCLEUS_NAME         "CORE.ZXFOUNDATION.NUCLEUS"
#define ZX_PARMFILE_NAME        "ETC.ZXFOUNDATION.PARM"

/// @brief Probe granularity: 1 MB frames.
#define MEM_PROBE_FRAME         (1UL << 20)

/// @brief Fallback memory ceiling when syssize= is absent from the parmfile.
///        Conservative: 512 MB matches the default Hercules MAINSIZE.
///        The real ceiling is supplied at runtime by parse_syssize().
#define MEM_PROBE_DEFAULT_MAX   (512UL << 20)

/// @brief Magic pattern written to test if a frame is usable RAM.
///        Two distinct values are written and read back to rule out
///        address-line faults (a single value could match a floating bus).
#define MEM_PROBE_PATTERN_A     UINT64_C(0xA5A5A5A5A5A5A5A5)
#define MEM_PROBE_PATTERN_B     UINT64_C(0x5A5A5A5A5A5A5A5A)

static zxfl_boot_protocol_t  s_proto;
static zxfl_mem_region_t     s_mem_map[ZXFL_MEM_MAP_MAX];
static zxfl_cpu_info_t       s_cpu_map[ZXFL_CPU_MAP_MAX];
static char                  s_cmdline[512];

/// @brief Kernel initial stack — 16 KB, loader-allocated.
///        The opaque frame is written just below s_kernel_stack_top.
static uint8_t s_kernel_stack[16384] __attribute__((aligned(16)));
#define s_kernel_stack_top (s_kernel_stack + sizeof(s_kernel_stack))

/// @brief Configure CR0 and CR6 for kernel entry.
///
///        CR0: We read the current value and clear only the bits the kernel
///        must manage itself:
///          - Bit 45 (AFP register control): cleared — kernel enables AFP.
///          - Bit 46 (vector facility control): cleared — kernel enables VX.
///          - Bit 36 (SSM suppression): cleared — kernel uses SSM freely.
///        All other CR0 bits are preserved from the loader's state.
///
///        CR6: Clear all I/O interrupt subclass masks.  The kernel starts
///        with no I/O interrupts enabled; it enables them per-subchannel.
///
///        CR14: Clear all machine-check masks.  The kernel installs its
///        own machine-check handler before enabling MCH interrupts.
static void setup_control_regs(void) {
    uint64_t cr0;
    __asm__ volatile ("stctg 0,0,%0" : "=Q" (cr0));

    cr0 &= ~(UINT64_C(1) << (63U - 45U));  // AFP
    cr0 &= ~(UINT64_C(1) << (63U - 46U));  // VX
    cr0 &= ~(UINT64_C(1) << (63U - 36U));  // SSM suppression

    __asm__ volatile ("lctlg 0,0,%0" :: "Q" (cr0));

    const uint64_t cr6  = 0;
    const uint64_t cr14 = 0;
    __asm__ volatile ("lctlg  6, 6,%0" :: "Q" (cr6));
    __asm__ volatile ("lctlg 14,14,%0" :: "Q" (cr14));
}

/// @brief Snapshot CR0 and CR14 after setup.
static void snapshot_control_regs(zxfl_boot_protocol_t *proto) {
    __asm__ volatile ("stctg  0, 0,%0" : "=Q" (proto->cr0_snapshot));
    __asm__ volatile ("stctg 14,14,%0" : "=Q" (proto->cr14_snapshot));
}

/// @param map          Output memory map array.
/// @param max          Maximum entries.
/// @param kernel_start Physical start of the loaded kernel.
/// @param kernel_end   Physical end (exclusive) of the loaded kernel.
/// @param mem_limit    Physical address ceiling for probing (from syssize=).
///                     Must be a multiple of MEM_PROBE_FRAME.
/// @return Number of entries written.
static uint32_t probe_memory(zxfl_mem_region_t *map, uint32_t max,
                             uint64_t kernel_start, uint64_t kernel_end,
                             uint64_t mem_limit) {
    uint32_t count = 0;

    if (count < max) {
        map[count].base   = 0x0;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type   = ZXFL_MEM_RESERVED;
        map[count]._pad   = 0;
        count++;
    }

    if (count < max) {
        map[count].base   = MEM_PROBE_FRAME;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type   = ZXFL_MEM_LOADER;
        map[count]._pad   = 0;
        count++;
    }

    for (uint64_t frame = 2UL * MEM_PROBE_FRAME;
         frame < mem_limit && count < max;
         frame += MEM_PROBE_FRAME) {

        volatile uint64_t *probe = (volatile uint64_t *)frame;

        const uint64_t saved = *probe;

        *probe = MEM_PROBE_PATTERN_A;
        const bool a_ok = (*probe == MEM_PROBE_PATTERN_A);

        *probe = MEM_PROBE_PATTERN_B;
        const bool b_ok = (*probe == MEM_PROBE_PATTERN_B);

        *probe = saved;

        if (!a_ok || !b_ok)
            break;  // First non-responding frame = end of RAM.

        uint32_t type = ZXFL_MEM_USABLE;
        if (frame >= kernel_start && frame < kernel_end)
            type = ZXFL_MEM_KERNEL;

        if (count > 0 &&
            map[count - 1].type == type &&
            map[count - 1].base + map[count - 1].length == frame) {
            map[count - 1].length += MEM_PROBE_FRAME;
        } else {
            map[count].base   = frame;
            map[count].length = MEM_PROBE_FRAME;
            map[count].type   = type;
            map[count]._pad   = 0;
            count++;
        }
    }

    return count;
}

/// @brief Sum all ZXFL_MEM_USABLE bytes in the map.
static uint64_t sum_usable_ram(const zxfl_mem_region_t *map, uint32_t count) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type == ZXFL_MEM_USABLE || map[i].type == ZXFL_MEM_KERNEL)
            total += map[i].length;
    }
    return total;
}

/// @brief Read the kernel command line from DASD into s_cmdline.
///        Silently leaves s_cmdline empty if the dataset is absent.
/// @param schid Subchannel ID.
static void load_parmfile(uint32_t schid) {
    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, ZX_PARMFILE_NAME, &ext) < 0) {
        print("zxfl01: parmfile etc.zxfoundation.parm requested but not found in vtoc\n");
        return;
    }

    static uint8_t parm_block[DASD_BLOCK_SIZE];
    if (dasd_read_record(schid, ext.begin_cyl, ext.begin_head, 1,
                         CCW_CMD_READ_DATA, parm_block, DASD_BLOCK_SIZE) < 0)
        return;

    uint32_t i = 0;
    while (i < sizeof(s_cmdline) - 1U) {
        const uint8_t c = parm_block[i];
        if (c == 0 || c == '\n' || c == '\r')
            break;
        s_cmdline[i] = (char)c;
        i++;
    }
    s_cmdline[i] = '\0';
}

/// @brief Transfer control to the kernel.
/// @param proto  Pointer to the filled boot protocol.
/// @param entry  Kernel entry point.
[[noreturn]] __attribute__((noinline)) static void jump_to_kernel(zxfl_boot_protocol_t *proto,
                                        uint64_t entry) {
    register uint64_t r2  __asm__("2")  = (uint64_t)proto;
    register uint64_t r14 __asm__("14") = entry;
    __asm__ volatile ("br %[entry]" :: [entry] "r"(r14), "r"(r2) : "memory");
    __builtin_unreachable();
}

/// @brief Stage 2 main entry point.  Called from entry.S with R2 = schid.
/// @param schid Subchannel ID of the IPL device.
[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader - core.zxfoundationloader01.sys\n");
    s_proto.stfle_count = stfle_detect(s_proto.stfle_fac, STFLE_MAX_DWORDS);
    s_proto.flags |= ZXFL_FLAG_STFLE;

    const uint16_t bsp_addr = zxfl_smp_current_cpu_addr();
    s_proto.cpu_count    = zxfl_smp_enumerate(s_cpu_map, ZXFL_CPU_MAP_MAX);
    s_proto.bsp_cpu_addr = (uint32_t)bsp_addr;
    s_proto.cpu_map_addr = (uint64_t)(uintptr_t)s_cpu_map;
    s_proto.flags |= ZXFL_FLAG_SMP;
    zxfl_smp_stop_aps(s_cpu_map, s_proto.cpu_count, bsp_addr);

    setup_control_regs();

    load_parmfile(schid);
    s_proto.cmdline_addr = (uint64_t)(uintptr_t)s_cmdline;
    s_proto.cmdline_len  = 0;
    for (uint32_t i = 0; s_cmdline[i] != '\0'; i++)
        s_proto.cmdline_len++;
    s_proto.flags |= ZXFL_FLAG_CMDLINE;

    uint64_t mem_limit = parse_syssize(s_cmdline, s_proto.cmdline_len);
    if (mem_limit == 0) {
        mem_limit = MEM_PROBE_DEFAULT_MAX;
        print("zxfl01: syssize= not set; defaulting to 512 MB probe ceiling\n");
    }

    dscb1_extent_t kernel_ext;
    if (dasd_find_dataset(schid, ZX_NUCLEUS_NAME, &kernel_ext) < 0)
        panic("zxfl01: core.zxfoundation.nucleus not found in vtoc\n");

    uint64_t entry_point = 0;
    uint32_t load_base   = 0;
    uint32_t load_size   = 0;

    print("zxfl01: preparing core.zxfoundation.nucleus\n");
    if (zxfl_load_elf64(schid, &kernel_ext,
                        &entry_point, &load_base, &load_size,
                        ZXVL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid)) < 0)
        panic("zxfl01: core.zxfoundation.nucleus load error\n");

    s_proto.kernel_phys_start = (uint64_t)load_base;
    s_proto.kernel_phys_end   = (uint64_t)load_base + (uint64_t)load_size;
    s_proto.kernel_entry      = entry_point;

    s_proto.mem_map_count = probe_memory(
        s_mem_map, ZXFL_MEM_MAP_MAX,
        s_proto.kernel_phys_start,
        s_proto.kernel_phys_end,
        mem_limit
    );
    s_proto.mem_map_addr    = (uint64_t)(uintptr_t)s_mem_map;
    s_proto.mem_total_bytes = sum_usable_ram(s_mem_map, s_proto.mem_map_count);
    s_proto.flags |= ZXFL_FLAG_MEM_MAP;

    s_proto.lowcore_phys = ZXFL_LOWCORE_PHYS;
    s_proto.flags |= ZXFL_FLAG_LOWCORE;

    s_proto.magic          = ZXFL_MAGIC;
    s_proto.version        = ZXFL_VERSION_3;
    s_proto.loader_major   = ZXFL_LOADER_MAJOR;
    s_proto.loader_minor   = ZXFL_LOADER_MINOR;
    s_proto.loader_timestamp = ZXVL_BUILD_TS;
    s_proto.loader_build_id  = 0;
    s_proto.ipl_schid      = schid;
    s_proto.ipl_dev_type   = 0x3390U;
    s_proto.ipl_dev_model  = 0x000CU;  // 3390-12

    s_proto.binding_token = ZXVL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid);

    {
        auto frame = (uint64_t *)((uint8_t *)s_kernel_stack_top - ZXVL_FRAME_SIZE);
        frame[0] = ZXVL_FRAME_MAGIC_A;
        frame[1] = ZXVL_FRAME_MAGIC_B ^ s_proto.binding_token;
        for (uint32_t i = 2; i < ZXVL_FRAME_SIZE / sizeof(uint64_t); i++)
            frame[i] = 0;
        s_proto.kernel_stack_top = (uint64_t)(uintptr_t)frame;
    }

    snapshot_control_regs(&s_proto);

    print("zxfl01: protocol completion complete\n");
    print("zxfl01: launching core.zxfoundation.nucleus\n");
    jump_to_kernel(&s_proto, entry_point);
}
