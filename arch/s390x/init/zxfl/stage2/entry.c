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
#include <zxfoundation/zxconfig.h>

#define ZX_NUCLEUS_NAME         "CORE.ZXFOUNDATION.NUCLEUS"
#define ZX_PARMFILE_NAME        "ETC.ZXFOUNDATION.PARM"

#define MEM_PROBE_FRAME         (1UL << 20)
#define MEM_PROBE_DEFAULT_MAX   (512UL << 20)
#define MEM_PROBE_PATTERN_A     UINT64_C(0xA5A5A5A5A5A5A5A5)
#define MEM_PROBE_PATTERN_B     UINT64_C(0x5A5A5A5A5A5A5A5A)

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

static uint32_t probe_memory(zxfl_mem_region_t *map, uint32_t max,
                             uint64_t kernel_start, uint64_t kernel_end,
                             uint64_t mem_limit) {
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

    uint32_t num_nodes = 1;
    if (s_proto.flags & ZXFL_FLAG_SMP) {
        for (uint32_t i = 0; i < s_proto.cpu_count; i++) {
            if (s_proto.cpu_map[i].numa_node + 1 > num_nodes) {
                num_nodes = s_proto.cpu_map[i].numa_node + 1;
            }
        }
    }

    uint64_t actual_limit = mem_limit;
    if (actual_limit == PARMFILE_SYSSIZE_INFINITE) {
        // Run a fast pre-probe pass to determine actual physical RAM size
        uint64_t probe_limit = 0;
        for (uint64_t frame = 2UL * MEM_PROBE_FRAME; ; frame += MEM_PROBE_FRAME) {
            volatile uint64_t *probe = (volatile uint64_t *) frame;
            const uint64_t saved = *probe;
            *probe = MEM_PROBE_PATTERN_A;
            const bool a_ok = (*probe == MEM_PROBE_PATTERN_A);
            *probe = MEM_PROBE_PATTERN_B;
            const bool b_ok = (*probe == MEM_PROBE_PATTERN_B);
            *probe = saved;
            if (!a_ok || !b_ok) {
                probe_limit = frame;
                break;
            }
        }
        actual_limit = probe_limit;
    }

    uint64_t node_chunk = actual_limit / num_nodes;
    uint64_t temp = node_chunk;
    uint64_t chunk_align = 256ULL * 1024 * 1024;
    while (temp >= chunk_align * 2) {
        chunk_align *= 2;
    }
    node_chunk = chunk_align;

    for (uint64_t frame = 2UL * MEM_PROBE_FRAME;
         frame < actual_limit && count < max;
         frame += MEM_PROBE_FRAME) {
        volatile uint64_t *probe = (volatile uint64_t *) frame;
        const uint64_t saved = *probe;
        *probe = MEM_PROBE_PATTERN_A;
        const bool a_ok = (*probe == MEM_PROBE_PATTERN_A);
        *probe = MEM_PROBE_PATTERN_B;
        const bool b_ok = (*probe == MEM_PROBE_PATTERN_B);
        *probe = saved;
        if (!a_ok || !b_ok) break;
        
        uint32_t type = ZXFL_MEM_USABLE;
        if (frame >= kernel_start && frame < kernel_end) type = ZXFL_MEM_KERNEL;
        
        uint8_t node = (frame / node_chunk) % 4;
        if (count > 0 && map[count - 1].type == type && map[count - 1].numa_node == node && map[count - 1].base + map[count - 1].length == frame) {
            map[count - 1].length += MEM_PROBE_FRAME;
        } else {
            map[count].base = frame;
            map[count].length = MEM_PROBE_FRAME;
            map[count].type = type;
            map[count].numa_node = node;
            count++;
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
                              uint64_t start, uint64_t end, uint32_t type, uint64_t node_chunk) {
    uint64_t curr = start;
    while (curr < end && *count < max) {
        uint64_t next_boundary = (curr + node_chunk) & ~(node_chunk - 1);
        uint64_t chunk_end = (next_boundary < end) ? next_boundary : end;
        
        map[*count].base = curr;
        map[*count].length = chunk_end - curr;
        map[*count].type = type;
        map[*count].numa_node = (curr / node_chunk) % 4;
        (*count)++;
        
        curr = chunk_end;
    }
}

static uint32_t detect_memory(zxfl_mem_region_t *map, uint32_t max,
                              uint64_t kernel_start, uint64_t kernel_end,
                              uint64_t mem_limit) {
    uint64_t sclp_size = 0;
    uint32_t count = 0;

    if (arch_sclp_early_get_memsize(&sclp_size) == 0) {
        print("zxfl01: memory detected via sclp: ");
        diag_print_hex8((uint32_t)(sclp_size >> 32));
        diag_print_hex8((uint32_t)sclp_size);
        print(" bytes\n");

        if (sclp_size > mem_limit) sclp_size = mem_limit;

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
        
        uint64_t usable_start = 2UL * MEM_PROBE_FRAME;
        if (usable_start < sclp_size) {
            uint32_t num_nodes = 1;
            if (s_proto.flags & ZXFL_FLAG_SMP) {
                for (uint32_t i = 0; i < s_proto.cpu_count; i++) {
                    if (s_proto.cpu_map[i].numa_node + 1 > num_nodes) {
                        num_nodes = s_proto.cpu_map[i].numa_node + 1;
                    }
                }
            }

            uint64_t node_chunk = sclp_size / num_nodes;
            uint64_t temp = node_chunk;
            uint64_t chunk_align = 256ULL * 1024 * 1024;
            while (temp >= chunk_align * 2) {
                chunk_align *= 2;
            }
            node_chunk = chunk_align;

            // Basic split for kernel if it falls within the range
            if (kernel_start >= usable_start && kernel_end <= sclp_size) {
                 add_usable_region(map, &count, max, usable_start, kernel_start, ZXFL_MEM_USABLE, node_chunk);
                 add_usable_region(map, &count, max, kernel_start, kernel_end, ZXFL_MEM_KERNEL, node_chunk);
                 add_usable_region(map, &count, max, kernel_end, sclp_size, ZXFL_MEM_USABLE, node_chunk);
            } else {
                 add_usable_region(map, &count, max, usable_start, sclp_size, ZXFL_MEM_USABLE, node_chunk);
            }
        }
        return count;
    }

    print("zxfl01: sclp detection failed, falling back to manual probe\n");
    return probe_memory(map, max, kernel_start, kernel_end, mem_limit);
}

[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader - core.zxfoundationloader01.sys\n");
    s_proto.stfle_count = stfle_detect(s_proto.stfle_fac, STFLE_MAX_DWORDS);
    s_proto.flags |= ZXFL_FLAG_STFLE;
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

    zxvl_verify_nucleus_checksums(s_proto.cksum_table_phys);
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
