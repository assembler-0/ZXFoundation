// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.c
//
// Stage 2 main logic.  Runs fully in 64-bit z/Architecture mode.
//
// Boot sequence:
//   1.  Lowcore setup — install safe disabled-wait new PSWs.
//   2.  STFLE — detect all CPU facilities (up to 32 dwords).
//   3.  SMP enumeration — find all CPUs via STSI; stop APs.
//   4.  Memory probe — walk physical memory in 1 MB frames to build map.
//   5.  Control register setup — CR0, CR6 for kernel entry state.
//   6.  Parmfile read — ETC.ZXFOUNDATION.PARM from DASD.
//   7.  ELF64 kernel load — CORE.ZXFOUNDATION.NUCLEUS from DASD.
//   8.  Protocol fill — populate zxfl_boot_protocol_t v3.
//   9.  Binding token — compute ZXFL_SEED ^ stfle_fac[0] ^ schid.
//  10.  Jump to kernel — R2 = &proto, R14 = entry_point.

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxfl_private.h>
#include <arch/s390x/init/zxfl/lowcore.h>
#include <arch/s390x/init/zxfl/smp.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

// ---------------------------------------------------------------------------
// Dataset names
// ---------------------------------------------------------------------------
#define ZX_NUCLEUS_NAME         "CORE.ZXFOUNDATION.NUCLEUS"
#define ZX_PARMFILE_NAME        "ETC.ZXFOUNDATION.PARM"

// ---------------------------------------------------------------------------
// Memory probe configuration
// ---------------------------------------------------------------------------

/// @brief Probe granularity: 1 MB frames.
#define MEM_PROBE_FRAME         (1UL << 20)

/// @brief Maximum physical address to probe — must not exceed MAINSIZE.
///        Set to match hercules.cnf MAINSIZE (512 MB).
#define MEM_PROBE_MAX           (512UL << 20)

/// @brief Magic pattern written to test if a frame is usable RAM.
///        Two distinct values are written and read back to rule out
///        address-line faults (a single value could match a floating bus).
#define MEM_PROBE_PATTERN_A     UINT64_C(0xA5A5A5A5A5A5A5A5)
#define MEM_PROBE_PATTERN_B     UINT64_C(0x5A5A5A5A5A5A5A5A)

// ---------------------------------------------------------------------------
// Static storage — all protocol auxiliary data lives here.
// Static allocation is mandatory in a freestanding environment.
// ---------------------------------------------------------------------------
static zxfl_boot_protocol_t  s_proto;
static zxfl_mem_region_t     s_mem_map[ZXFL_MEM_MAP_MAX];
static zxfl_cpu_info_t       s_cpu_map[ZXFL_CPU_MAP_MAX];
static char                  s_cmdline[512];

/// @brief Kernel initial stack — 16 KB, loader-allocated.
///        The opaque frame is written just below s_kernel_stack_top.
static uint8_t s_kernel_stack[16384] __attribute__((aligned(16)));
#define s_kernel_stack_top (s_kernel_stack + sizeof(s_kernel_stack))

// ---------------------------------------------------------------------------
// Control register setup
// ---------------------------------------------------------------------------

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

    // Clear AFP (bit 45), vector (bit 46), SSM suppression (bit 36).
    // Bit numbering: bit 0 = MSB of the 64-bit register.
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

// ---------------------------------------------------------------------------
// Memory probe
// ---------------------------------------------------------------------------

///
///        We write two distinct 64-bit patterns to offset 0 of each 1 MB
///        frame and verify they read back correctly.  A frame that does not
///        respond correctly is either not present or not usable RAM.
///
///        Frames occupied by the loader itself (0x0–0x3FFFF for stage1+2)
///        and the kernel load area are marked accordingly.
///
///        The probe is destructive to the test location within each frame,
///        but we only write to offset 0 of each frame — well below any
///        loaded code or data.
///
/// @param map          Output memory map array.
/// @param max          Maximum entries.
/// @param kernel_start Physical start of the loaded kernel.
/// @param kernel_end   Physical end (exclusive) of the loaded kernel.
/// @return Number of entries written.
static uint32_t probe_memory(zxfl_mem_region_t *map, uint32_t max,
                             uint64_t kernel_start, uint64_t kernel_end) {
    uint32_t count = 0;

    // Frame 0 (0x0–0xFFFFF) contains the lowcore and stage1 code.
    // Mark it reserved unconditionally — we never probe it.
    if (count < max) {
        map[count].base   = 0x0;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type   = ZXFL_MEM_RESERVED;
        map[count]._pad   = 0;
        count++;
    }

    // Frame 1 (0x100000–0x1FFFFF) contains stage2 (loaded at 0x20000).
    // Mark it as loader memory.
    if (count < max) {
        map[count].base   = MEM_PROBE_FRAME;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type   = ZXFL_MEM_LOADER;
        map[count]._pad   = 0;
        count++;
    }

    // Probe frames from 2 MB upward.
    for (uint64_t frame = 2UL * MEM_PROBE_FRAME;
         frame < MEM_PROBE_MAX && count < max;
         frame += MEM_PROBE_FRAME) {

        volatile uint64_t *probe = (volatile uint64_t *)frame;

        // Save original value (may be valid data if we're re-probing).
        const uint64_t saved = *probe;

        *probe = MEM_PROBE_PATTERN_A;
        const bool a_ok = (*probe == MEM_PROBE_PATTERN_A);

        *probe = MEM_PROBE_PATTERN_B;
        const bool b_ok = (*probe == MEM_PROBE_PATTERN_B);

        // Restore original value to avoid corrupting anything already loaded.
        *probe = saved;

        if (!a_ok || !b_ok)
            break;  // First non-responding frame = end of RAM.

        // Determine region type.
        uint32_t type = ZXFL_MEM_USABLE;
        if (frame >= kernel_start && frame < kernel_end)
            type = ZXFL_MEM_KERNEL;

        // Merge with previous entry if same type and contiguous.
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

// ---------------------------------------------------------------------------
// Parmfile
// ---------------------------------------------------------------------------

/// @brief Read the kernel command line from DASD into s_cmdline.
///        Silently leaves s_cmdline empty if the dataset is absent.
/// @param schid Subchannel ID.
static void load_parmfile(uint32_t schid) {
    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, ZX_PARMFILE_NAME, &ext) < 0)
        return;

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

// ---------------------------------------------------------------------------
// Kernel jump
// ---------------------------------------------------------------------------

/// @brief Transfer control to the kernel.
///
///        Register state at kernel entry (per ZXFL ABI):
///          R2  = physical address of zxfl_boot_protocol_t
///          R14 = kernel entry point (used as branch target)
///          All other GPRs = 0 (cleared for a clean slate)
///          PSW = 64-bit, DAT off, all interrupts masked
///
///        The PSW at kernel entry has interrupts masked because the kernel
///        must install its own interrupt handlers before enabling any.
///        DAT is off because the kernel builds its own page tables.
///
/// @param proto  Pointer to the filled boot protocol.
/// @param entry  Kernel entry point.
[[noreturn]] __attribute__((noinline)) static void jump_to_kernel(zxfl_boot_protocol_t *proto,
                                        uint64_t entry) {
    register uint64_t r2  __asm__("2")  = (uint64_t)proto;
    register uint64_t r14 __asm__("14") = entry;
    // Branch directly. Zeroing caller-saved GPRs before the branch is
    // unsafe when inlined: zeroing r13 (frame pointer) corrupts GCC's
    // stack frame and clobbers r2 before the branch executes.
    // The kernel establishes its own register state immediately on entry.
    __asm__ volatile ("br %[entry]" :: [entry] "r"(r14), "r"(r2) : "memory");
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// Stage 2 entry point
// ---------------------------------------------------------------------------

/// @brief Stage 2 main entry point.  Called from entry.S with R2 = schid.
/// @param schid Subchannel ID of the IPL device.
[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader - core.zxfoundationloader01.sys\n");

    // ---- 1. Lowcore setup — done in entry.S before BSS zero ----

    // ---- 2. STFLE ----
    s_proto.stfle_count = stfle_detect(s_proto.stfle_fac, STFLE_MAX_DWORDS);
    s_proto.flags |= ZXFL_FLAG_STFLE;

    // ---- 3. SMP enumeration ----
    const uint16_t bsp_addr = zxfl_smp_current_cpu_addr();
    s_proto.cpu_count    = zxfl_smp_enumerate(s_cpu_map, ZXFL_CPU_MAP_MAX);
    s_proto.bsp_cpu_addr = (uint32_t)bsp_addr;
    s_proto.cpu_map_addr = (uint64_t)(uintptr_t)s_cpu_map;
    s_proto.flags |= ZXFL_FLAG_SMP;
    zxfl_smp_stop_aps(s_cpu_map, s_proto.cpu_count, bsp_addr);

    // ---- 4. Control register setup ----
    setup_control_regs();

    // ---- 5. Parmfile ----
    load_parmfile(schid);
    s_proto.cmdline_addr = (uint64_t)(uintptr_t)s_cmdline;
    s_proto.cmdline_len  = 0;
    for (uint32_t i = 0; s_cmdline[i] != '\0'; i++)
        s_proto.cmdline_len++;
    s_proto.flags |= ZXFL_FLAG_CMDLINE;

    // ---- 6. ELF64 kernel load ----
    dscb1_extent_t kernel_ext;
    if (dasd_find_dataset(schid, ZX_NUCLEUS_NAME, &kernel_ext) < 0)
        panic("zxfl01: core.zxfoundation.nucleus not found in vtoc\n");

    uint64_t entry_point = 0;
    uint32_t load_base   = 0;
    uint32_t load_size   = 0;

    if (zxfl_load_elf64(schid, &kernel_ext,
                        &entry_point, &load_base, &load_size,
                        ZXFL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid)) < 0)
        panic("zxfl01: core.zxfoundation.nucleus load error\n");

    s_proto.kernel_phys_start = (uint64_t)load_base;
    s_proto.kernel_phys_end   = (uint64_t)load_base + (uint64_t)load_size;
    s_proto.kernel_entry      = entry_point;

    // ---- 7. Memory probe ----
    s_proto.mem_map_count = probe_memory(
        s_mem_map, ZXFL_MEM_MAP_MAX,
        s_proto.kernel_phys_start,
        s_proto.kernel_phys_end
    );
    s_proto.mem_map_addr    = (uint64_t)(uintptr_t)s_mem_map;
    s_proto.mem_total_bytes = sum_usable_ram(s_mem_map, s_proto.mem_map_count);
    s_proto.flags |= ZXFL_FLAG_MEM_MAP;

    // ---- 8. Lowcore pointer ----
    s_proto.lowcore_phys = ZXFL_LOWCORE_PHYS;
    s_proto.flags |= ZXFL_FLAG_LOWCORE;

    // ---- 9. Protocol header ----
    s_proto.magic          = ZXFL_MAGIC;
    s_proto.version        = ZXFL_VERSION_3;
    s_proto.loader_major   = ZXFL_LOADER_MAJOR;
    s_proto.loader_minor   = ZXFL_LOADER_MINOR;
    s_proto.loader_timestamp = ZXFL_BUILD_TS;
    s_proto.loader_build_id  = 0;
    s_proto.ipl_schid      = schid;
    s_proto.ipl_dev_type   = 0x3390U;
    s_proto.ipl_dev_model  = 0x000CU;  // 3390-12

    // ---- 10. Binding token ----
    s_proto.binding_token = ZXFL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid);

    // ---- 11. Loader-provided kernel stack + opaque frame ----
    // The opaque frame sits at the very top of the stack allocation.
    // magic_b ^ binding_token makes it unique per boot and per machine.
    // The kernel validates both words before trusting the boot protocol.
    {
        uint64_t *frame = (uint64_t *)((uint8_t *)s_kernel_stack_top - ZXFL_FRAME_SIZE);
        frame[0] = ZXFL_FRAME_MAGIC_A;
        frame[1] = ZXFL_FRAME_MAGIC_B ^ s_proto.binding_token;
        for (uint32_t i = 2; i < ZXFL_FRAME_SIZE / sizeof(uint64_t); i++)
            frame[i] = 0;
        s_proto.kernel_stack_top = (uint64_t)(uintptr_t)frame;
    }

    // ---- 12. CR snapshot ----
    snapshot_control_regs(&s_proto);

    print("zxfl01: launching core.zxfoundation.nucleus\n");
    jump_to_kernel(&s_proto, entry_point);
}
