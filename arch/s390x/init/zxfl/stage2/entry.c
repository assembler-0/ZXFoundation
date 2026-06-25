// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.c

#include <arch/s390x/init/zxfl/processor.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/sclp.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/dasd_eckd.h>
#include <arch/s390x/init/zxfl/dasd_fba.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <arch/s390x/init/zxfl/parmfile.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/mmu.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <zxfoundation/zxconfig.h>

#define ZX_NUCLEUS_NAME         "CORE.ZXFOUNDATION.NUCLEUS"
#define ZX_PARMFILE_NAME        "ETC.ZXFOUNDATION.PARM"

#define MEM_PROBE_FRAME         (1UL << 20)
#define MEM_PROBE_MAX_BYTES     (16ULL * 1024 * 1024 * 1024 * 1024)

static zxfl_boot_protocol_t s_proto;
static zxfl_mem_region_t s_mem_map[ZXFL_MEM_MAP_MAX];
static char s_cmdline[512];

static uint8_t s_kernel_stack[16384] __attribute__((aligned(16)));
#define s_kernel_stack_top (s_kernel_stack + sizeof(s_kernel_stack))

static void setup_control_regs(void) {
    uint64_t cr0;
    arch_ctl_store(cr0, 0, 0);
    cr0 &= ~(UINT64_C(1) << (63U - 45U));
    cr0 &= ~(UINT64_C(1) << (63U - 46U));
    cr0 &= ~(UINT64_C(1) << (63U - 36U));
    arch_ctl_load(cr0, 0, 0);
    const uint64_t cr6 = 0;
    const uint64_t cr14 = 0;
    arch_ctl_load(cr6, 6, 6);
    arch_ctl_load(cr14, 14, 14);
}

static void snapshot_control_regs(zxfl_boot_protocol_t *proto) {
    arch_ctl_store(proto->cr13_snapshot, 13, 13);
}

static uint32_t s_recs_per_trk = 12;
static uint16_t s_heads_per_cyl = DASD_3390_HEADS_PER_CYL;

static uint32_t zxfl_discovered_numa_nodes(void) {
    uint32_t num_nodes = 1;
    if (s_proto.flags & ZXFL_FLAG_SMP) {
        for (uint32_t i = 0; i < s_proto.cpu_count; i++) {
            uint32_t candidate = (uint32_t)s_proto.cpu_map[i].numa_node + 1U;
            if (candidate > num_nodes) num_nodes = candidate;
        }
    }
    if (num_nodes == 0U) num_nodes = 1U;
    if (num_nodes > 255U) num_nodes = 255U;
    return num_nodes;
}

static uint8_t zxfl_numa_node_for_phys(uint64_t phys, uint64_t node_chunk, uint32_t num_nodes) {
    if (node_chunk == 0U || num_nodes == 0U) return 0U;
    return (uint8_t)((phys / node_chunk) % num_nodes);
}

/// @brief Probe the IPL device as ECKD or FBA and populate ipl_dev_type/model.
///
///        ECKD is tried first because 3390 is the dominant IPL device type.
///        If ECKD probe fails (Sense ID returns a non-ECKD type), FBA is tried.
///        On failure of both, the fields remain zero — the kernel must tolerate
///        this for virtual/emulated environments that don't implement Sense ID.
static void probe_ipl_device(uint32_t schid, zxfl_boot_protocol_t *proto) {
    dasd_eckd_geo_t eckd_geo;
    if (dasd_eckd_probe(schid, &eckd_geo) == 0) {
        proto->ipl_dev_type = eckd_geo.dev_type;
        proto->ipl_dev_model = eckd_geo.dev_model;
        s_recs_per_trk = eckd_geo.recs_per_trk;
        s_heads_per_cyl = eckd_geo.heads;
        print("zxfl01: ipl device: eckd\n");
        return;
    }

    dasd_fba_geo_t fba_geo;
    if (dasd_fba_probe(schid, &fba_geo) == 0) {
        proto->ipl_dev_type = fba_geo.dev_type;
        proto->ipl_dev_model = fba_geo.dev_model;
        print("zxfl01: ipl device: fba\n");
        return;
    }

    print("zxfl01: ipl device: unknown (sense id failed)\n");
}

static void add_usable_region(zxfl_mem_region_t *map, uint32_t *count, uint32_t max,
                              uint64_t start, uint64_t end, uint32_t type,
                              uint64_t node_chunk, uint32_t num_nodes);

/// @brief Test whether the real frame at @p addr is installed storage.
/// @param addr  Real address of the frame to test.
/// @return true if the storage is installed, false on addressing exception.
/// @warning Must run with DAT off and prefix zero (early loader context).
static bool probe_frame_present(uint64_t addr) {
    uint64_t present;
    __asm__ volatile(
        "   larl   %%r1,0f\n"           // r1 = fixup resume address
        "   stg    %%r1,0x1d8\n"        // program-new PSW: instruction address
        "   stg    %[mask],0x1d0\n"     // program-new PSW: mask (enabled, DAT off)
        "   lghi   %[pres],1\n"         // assume installed
        "   tprot  0(%[addr]),0\n"      // suppressed addressing exc -> fixup
        "   j      1f\n"
        "0: lghi   %[pres],0\n"         // fixup: addressing exception was taken
        "1:\n"
        : [pres] "=&d"(present)
        : [addr] "a"(addr), [mask] "d"(PSW_MASK_KERNEL)
        : "r1", "cc", "memory");
    return present != 0;
}

/// @brief Find the exclusive end of installed storage via a TPROT binary search. /// @return Exclusive physical end of installed RAM, in bytes.
/// @note Restores the disabled-wait program-check PSW before returning.
static uint64_t probe_mem_end_bytes(void) {
    uint64_t range  = MEM_PROBE_MAX_BYTES / MEM_PROBE_FRAME;
    uint64_t offset = 0;
    while (range > 1) {
        range >>= 1;
        uint64_t pivot = offset + range;
        if (probe_frame_present(pivot * MEM_PROBE_FRAME))
            offset = pivot;
    }
    // Restore the disabled-wait program-check PSW so a genuine later fault
    // halts the CPU instead of silently resuming at a stale fixup label.
    arch_psw_set_raw(PSW_LC_PROGRAM, PSW_MASK_DISABLED_WAIT,
                     0x000000000DEAD1D0ULL);
    return (offset + 1) * MEM_PROBE_FRAME;
}

/// @brief Query installed storage size via DIAG 0x260 (z/VM / emulator).
/// @param out_size  Receives installed storage size in bytes on success.
/// @return 0 on success, -1 if DIAG 0x260 is unavailable or reports nothing.
/// @warning Must run with DAT off and prefix zero (early loader context).
static int diag260_get_memsize(uint64_t *out_size) {
    // 16-byte-aligned extent descriptor pair {start, end} (PoP/z/VM quadword).
    static uint64_t extent[2] __attribute__((aligned(16)));
    extent[0] = 0;
    extent[1] = 0;

    register unsigned long buf __asm__("2") = (unsigned long)(uintptr_t)extent; // R1 even: address
    register unsigned long len __asm__("3") = (unsigned long)sizeof(extent);    // R1 odd:  length
    register unsigned long sub __asm__("4") = 0x10UL;                           // R3: subcode / count
    int present = 1;

    __asm__ volatile(
        "   larl   %%r1,0f\n"
        "   stg    %%r1,0x1d8\n"            // program-new PSW: fixup address
        "   stg    %[mask],0x1d0\n"         // program-new PSW: mask (enabled, DAT off)
        "   diag   %[buf],%[sub],0x260\n"   // DIAG R1=pair(buf,len), R3=subcode
        "   j      1f\n"
        "0: lhi    %[pres],0\n"             // fixup: DIAG raised a program interruption
        "1:\n"
        : [pres] "+d"(present), [buf] "+d"(buf), [len] "+d"(len), [sub] "+d"(sub)
        : [mask] "d"(PSW_MASK_KERNEL)
        : "r1", "cc", "memory");

    // Restore the disabled-wait program-check PSW.
    arch_psw_set_raw(PSW_LC_PROGRAM, PSW_MASK_DISABLED_WAIT,
                     0x000000000DEAD1D0ULL);

    if (!present)
        return -1;

    const uint64_t end = extent[1];   // highest addressable byte
    if (end == 0)
        return -1;
    *out_size = end + 1UL;
    return 0;
}

/// @brief Populate the boot memory map from a known total RAM size.
/// @return Number of regions written to @p map.
static uint32_t build_mem_map(zxfl_mem_region_t *map, uint32_t max,
                              uint64_t kernel_start, uint64_t kernel_end,
                              uint64_t total_size) {
    uint32_t count = 0;
    if (count < max) {
        map[count].base = 0x0;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type = ZXFL_MEM_RESERVED;
        map[count].numa_node = 0;
        count++;
    }
    if (count < max) {
        map[count].base = MEM_PROBE_FRAME;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type = ZXFL_MEM_LOADER;
        map[count].numa_node = 0;
        count++;
    }

    uint32_t num_nodes = zxfl_discovered_numa_nodes();

    uint64_t node_chunk = total_size / num_nodes;
    uint64_t chunk_align = 256ULL * 1024 * 1024;
    while (node_chunk >= chunk_align * 2) {
        chunk_align *= 2;
    }
    node_chunk = chunk_align;

    uint64_t usable_start = 2UL * MEM_PROBE_FRAME;
    if (usable_start < total_size) {
        if (kernel_start >= usable_start && kernel_end <= total_size) {
            add_usable_region(map, &count, max, usable_start, kernel_start, ZXFL_MEM_USABLE, node_chunk, num_nodes);
            add_usable_region(map, &count, max, kernel_start, kernel_end, ZXFL_MEM_KERNEL, node_chunk, num_nodes);
            add_usable_region(map, &count, max, kernel_end, total_size, ZXFL_MEM_USABLE, node_chunk, num_nodes);
        } else {
            add_usable_region(map, &count, max, usable_start, total_size, ZXFL_MEM_USABLE, node_chunk, num_nodes);
        }
    }
    return count;
}

static uint64_t sum_usable_ram(const zxfl_mem_region_t *map, uint32_t count) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type == ZXFL_MEM_USABLE || map[i].type == ZXFL_MEM_KERNEL)
            total += map[i].length;
    }
    return total;
}

static void load_parmfile(uint32_t schid) {
    dasd_dataset_t ds;
    if (dasd_find_dataset_extents(schid, ZX_PARMFILE_NAME, &ds) < 0) return;
    static uint8_t parm_block[DASD_BLOCK_SIZE];
    if (dasd_read_record(schid, ds.extents[0].begin_cyl, ds.extents[0].begin_head,
                         1, CCW_CMD_READ_DATA, parm_block, DASD_BLOCK_SIZE) < 0)
        return;
    uint32_t i = 0;
    while (i < sizeof(s_cmdline) - 1U) {
        const uint8_t c = parm_block[i];
        if (c == 0 || c == '\n' || c == '\r') break;
        s_cmdline[i] = (char) c;
        i++;
    }
    s_cmdline[i] = '\0';
}

static uint64_t load_modules(uint32_t schid, const char *cmdline, uint64_t phys_start) {
    uint64_t current_phys = phys_start;
    current_phys = (current_phys + 0xFFF) & ~0xFFFULL;

    s_proto.module_count = 0;

    const char *p = cmdline;
    while (*p) {
        const char *mod_key = "sysmodule=";
        bool match = true;
        for (int i = 0; i < 10; i++) {
            if (p[i] != mod_key[i]) {
                match = false;
                break;
            }
        }

        if (match) {
            p += 10;
            while (*p && *p != ' ' && *p != '\n') {
                char modname[45];
                int n = 0;
                while (*p && *p != ',' && *p != ' ' && *p != '\n' && n < 44) {
                    modname[n++] = *p++;
                }
                modname[n] = '\0';

                dscb1_extent_t ext;
                if (dasd_find_dataset(schid, modname, &ext) >= 0) {
                    print("zxfl01: loading module ");
                    print(modname);
                    print("\n");

                    uint32_t tracks = (ext.end_cyl - ext.begin_cyl) * s_heads_per_cyl + (
                                          ext.end_head - ext.begin_head) + 1;
                    uint32_t max_blocks = tracks * s_recs_per_trk;

                    static uint8_t mod_block[DASD_BLOCK_SIZE] __attribute__((aligned(DASD_BLOCK_SIZE)));
                    uint64_t loaded = 0;

                    for (uint32_t b = 0; b < max_blocks; b++) {
                        uint32_t c = ext.begin_cyl + (ext.begin_head + b / s_recs_per_trk) / s_heads_per_cyl;
                        uint32_t h = (ext.begin_head + b / s_recs_per_trk) % s_heads_per_cyl;
                        uint32_t r = (b % s_recs_per_trk) + 1;
                        if (dasd_read_record(schid, c, h, r, CCW_CMD_READ_DATA, mod_block, DASD_BLOCK_SIZE) < 0) {
                            break;
                        }
                        memcpy((void *) (uintptr_t) (current_phys + loaded), mod_block, DASD_BLOCK_SIZE);
                        loaded += DASD_BLOCK_SIZE;
                    }

                    if (s_proto.module_count < 16) {
                        for (int i = 0; i < 31 && modname[i]; i++)
                            s_proto.modules[s_proto.module_count].name[i] = modname[i];
                        s_proto.modules[s_proto.module_count].name[31] = '\0';
                        s_proto.modules[s_proto.module_count].phys_start = current_phys;
                        s_proto.modules[s_proto.module_count].size_bytes = loaded;
                        s_proto.module_count++;
                    }

                    current_phys += loaded;
                    current_phys = (current_phys + 0xFFF) & ~0xFFFULL;
                } else {
                    print("zxfl01: module not found: ");
                    print(modname);
                    print("\n");
                }

                if (*p == ',') p++;
            }
        } else {
            p++;
        }
    }
    return current_phys;
}

static void add_usable_region(zxfl_mem_region_t *map, uint32_t *count, uint32_t max,
                              uint64_t start, uint64_t end, uint32_t type,
                              uint64_t node_chunk, uint32_t num_nodes) {
    uint64_t curr = start;
    while (curr < end && *count < max) {
        uint64_t next_boundary = (curr + node_chunk) & ~(node_chunk - 1);
        uint64_t chunk_end = (next_boundary < end) ? next_boundary : end;

        map[*count].base      = curr;
        map[*count].length    = chunk_end - curr;
        map[*count].type      = type;
        map[*count].numa_node = zxfl_numa_node_for_phys(curr, node_chunk, num_nodes);
        (*count)++;

        curr = chunk_end;
    }
}

static uint32_t detect_memory(zxfl_mem_region_t *map, uint32_t max,
                              uint64_t kernel_start, uint64_t kernel_end,
                              uint64_t mem_limit) {
    uint64_t total = 0;

    if (arch_sclp_early_get_memsize(&total) == 0) {
        print("zxfl01: memory detected via sclp\n");
    }
    else if (diag260_get_memsize(&total) == 0) {
        print("zxfl01: memory detected via diag 0x260\n");
    }
    else {
        print("zxfl01: sclp/diag unavailable, probing storage via tprot\n");
        total = probe_mem_end_bytes();
    }

    if (mem_limit != PARMFILE_SYSSIZE_INFINITE && total > mem_limit)
        total = mem_limit;

    return build_mem_map(map, max, kernel_start, kernel_end, total);
}

[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader - core.zxfoundationloader01.sys\n");
    s_proto.stfle_count = stfle_detect(s_proto.stfle_fac, STFLE_MAX_DWORDS);
    s_proto.flags |= ZXFL_FLAG_STFLE;

    if (!stfle_has_facility(s_proto.stfle_fac, STFLE_BIT_ZARCH))
        panic("zxfl01: z/Architecture (facility 2) not available");
    if (!stfle_has_facility(s_proto.stfle_fac, STFLE_BIT_EIMM))
        panic("zxfl01: extended-immediate facility (21) required");
    if (!stfle_has_facility(s_proto.stfle_fac, STFLE_BIT_GEN_INST))
        panic("zxfl01: general-instructions-extension (25) required");
    if (!stfle_has_facility(s_proto.stfle_fac, STFLE_BIT_EDAT1))
        print("zxfl01: WARNING: EDAT-1 (facility 8) not available, using 4K pages\n");

    setup_control_regs();
    probe_ipl_device(schid, &s_proto);
    load_parmfile(schid);
    s_proto.cmdline_addr = (uintptr_t) s_cmdline;
    s_proto.cmdline_len = 0;
    for (uint32_t i = 0; s_cmdline[i] != '\0'; i++) s_proto.cmdline_len++;
    s_proto.flags |= ZXFL_FLAG_CMDLINE;
    uint64_t mem_limit = parse_syssize(s_cmdline, s_proto.cmdline_len);
    if (mem_limit == 0) mem_limit = PARMFILE_SYSSIZE_INFINITE;
    dasd_dataset_t kernel_ds;
    if (dasd_find_dataset_extents(schid, ZX_NUCLEUS_NAME, &kernel_ds) < 0) panic("zxfl01: nucleus not found");
    uint64_t entry_point = 0;
    uint64_t load_base = 0;
    uint64_t load_size = 0;
    print("zxfl01: preparing core.zxfoundation.nucleus\n");
    if (zxfl_load_elf64(schid, &kernel_ds, &s_proto, &entry_point, &load_base, &load_size,
                        ZXVL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid), s_recs_per_trk, s_heads_per_cyl) < 0)
        panic("zxfl01: load error");

    zxvl_verify_nucleus_checksums(s_proto.cksum_table);
    print("zxfl01: nucleus verified\n");

    s_proto.kernel_phys_start = load_base;
    s_proto.kernel_phys_end = load_modules(schid, s_cmdline, load_base + load_size);
    s_proto.kernel_entry = entry_point;

    print("zxfl01: detecting system (stsi/smp/tod)\n");
    zxfl_system_detect(&s_proto);

    {
        const uint64_t virt_off = CONFIG_KERNEL_VIRT_OFFSET;
        uint64_t phys_start = s_proto.kernel_phys_start;
        uint64_t phys_end = s_proto.kernel_phys_end;
        if (phys_start >= virt_off) phys_start -= virt_off;
        if (phys_end >= virt_off) phys_end -= virt_off;
        s_proto.mem_map_count = detect_memory(s_mem_map, ZXFL_MEM_MAP_MAX,
                                              phys_start, phys_end, mem_limit);
    }
    s_proto.mem_map_addr = (uint64_t) (uintptr_t) s_mem_map;
    s_proto.mem_total_bytes = sum_usable_ram(s_mem_map, s_proto.mem_map_count);
    s_proto.flags |= ZXFL_FLAG_MEM_MAP;
    s_proto.lowcore_phys = 0;
    s_proto.flags |= ZXFL_FLAG_LOWCORE;

    s_proto.magic = ZXFL_MAGIC;
    s_proto.version = ZXFL_VERSION_4;
    s_proto.loader_major = ZXFL_LOADER_MAJOR;
    s_proto.loader_minor = ZXFL_LOADER_MINOR;
    s_proto.loader_timestamp = ZXVL_BUILD_TS;
    s_proto.ipl_schid = schid;
    s_proto.binding_token = ZXVL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid);
    {
        auto frame = (uint64_t *) ((uint8_t *) s_kernel_stack_top - ZXVL_FRAME_SIZE);
        frame[0] = ZXVL_FRAME_MAGIC_A;
        frame[1] = ZXVL_FRAME_MAGIC_B ^ s_proto.binding_token;
        s_proto.kernel_stack_top = (uint64_t) (uintptr_t) frame;
    }
    snapshot_control_regs(&s_proto);

    print("zxfl01: enabling DAT and jumping to kernel\n");
    zxfl_mmu_setup_and_jump(entry_point, (uint64_t) (uintptr_t) &s_proto);
}
