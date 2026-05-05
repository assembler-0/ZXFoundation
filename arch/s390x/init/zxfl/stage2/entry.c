// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.c

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <arch/s390x/init/zxfl/parmfile.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/mmu.h>
#include <zxfoundation/zconfig.h>

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
    __asm__ volatile ("stctg 0,0,%0" : "=Q" (cr0));
    cr0 &= ~(UINT64_C(1) << (63U - 45U));
    cr0 &= ~(UINT64_C(1) << (63U - 46U));
    cr0 &= ~(UINT64_C(1) << (63U - 36U));
    __asm__ volatile ("lctlg 0,0,%0" :: "Q" (cr0));
    const uint64_t cr6 = 0;
    const uint64_t cr14 = 0;
    __asm__ volatile ("lctlg  6, 6,%0" :: "Q" (cr6));
    __asm__ volatile ("lctlg 14,14,%0" :: "Q" (cr14));
}

static void snapshot_control_regs(zxfl_boot_protocol_t *proto) {
    __asm__ volatile ("stctg 14,14,%0" : "=Q" (proto->cr14_snapshot));
}

static uint32_t probe_memory(zxfl_mem_region_t *map, uint32_t max,
                             uint64_t kernel_start, uint64_t kernel_end,
                             uint64_t mem_limit) {
    uint32_t count = 0;
    if (count < max) {
        map[count].base = 0x0;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type = ZXFL_MEM_RESERVED;
        count++;
    }
    if (count < max) {
        map[count].base = MEM_PROBE_FRAME;
        map[count].length = MEM_PROBE_FRAME;
        map[count].type = ZXFL_MEM_LOADER;
        count++;
    }
    for (uint64_t frame = 2UL * MEM_PROBE_FRAME;
         frame < mem_limit && count < max;
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
        if (count > 0 && map[count - 1].type == type && map[count - 1].base + map[count - 1].length == frame) {
            map[count - 1].length += MEM_PROBE_FRAME;
        } else {
            map[count].base = frame;
            map[count].length = MEM_PROBE_FRAME;
            map[count].type = type;
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
    dscb1_extent_t ext;
    if (dasd_find_dataset(schid, ZX_PARMFILE_NAME, &ext) < 0) return;
    static uint8_t parm_block[DASD_BLOCK_SIZE];
    if (dasd_read_record(schid, ext.begin_cyl, ext.begin_head, 1, CCW_CMD_READ_DATA, parm_block, DASD_BLOCK_SIZE) < 0)
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

                    uint32_t tracks = (ext.end_cyl - ext.begin_cyl) * DASD_3390_HEADS_PER_CYL + (
                                          ext.end_head - ext.begin_head) + 1;
                    uint32_t max_blocks = tracks * 12; // ZXVL_RECS_PER_TRACK

                    static uint8_t mod_block[DASD_BLOCK_SIZE] __attribute__((aligned(DASD_BLOCK_SIZE)));
                    uint64_t loaded = 0;

                    for (uint32_t b = 0; b < max_blocks; b++) {
                        uint32_t c = ext.begin_cyl + (ext.begin_head + b / 12) / DASD_3390_HEADS_PER_CYL;
                        uint32_t h = (ext.begin_head + b / 12) % DASD_3390_HEADS_PER_CYL;
                        uint32_t r = (b % 12) + 1;
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

[[noreturn]] void zxfl01_entry(const uint32_t schid) {
    print("zxfl01: ZXFoundationLoader - core.zxfoundationloader01.sys\n");
    s_proto.stfle_count = stfle_detect(s_proto.stfle_fac, STFLE_MAX_DWORDS);
    s_proto.flags |= ZXFL_FLAG_STFLE;
    setup_control_regs();
    load_parmfile(schid);
    s_proto.cmdline_addr = (uintptr_t) s_cmdline;
    s_proto.cmdline_len = 0;
    for (uint32_t i = 0; s_cmdline[i] != '\0'; i++) s_proto.cmdline_len++;
    s_proto.flags |= ZXFL_FLAG_CMDLINE;
    uint64_t mem_limit = parse_syssize(s_cmdline, s_proto.cmdline_len);
    if (mem_limit == 0) mem_limit = MEM_PROBE_DEFAULT_MAX;
    dscb1_extent_t kernel_ext;
    if (dasd_find_dataset(schid, ZX_NUCLEUS_NAME, &kernel_ext) < 0) panic("zxfl01: nucleus not found");
    uint64_t entry_point = 0;
    uint64_t load_base = 0;
    uint64_t load_size = 0;
    print("zxfl01: preparing core.zxfoundation.nucleus\n");
    if (zxfl_load_elf64(schid, &kernel_ext, &entry_point, &load_base, &load_size,
                        ZXVL_COMPUTE_TOKEN(s_proto.stfle_fac[0], schid)) < 0)
        panic("zxfl01: load error");

    zxvl_verify_nucleus_checksums(load_base);
    print("zxfl01: nucleus verified\n");

    s_proto.kernel_phys_start = load_base;
    s_proto.kernel_phys_end = load_modules(schid, s_cmdline, load_base + load_size);
    s_proto.kernel_entry = entry_point;

    {
        const uint64_t virt_off = CONFIG_KERNEL_VIRT_OFFSET;
        uint64_t phys_start = s_proto.kernel_phys_start;
        uint64_t phys_end   = s_proto.kernel_phys_end;
        if (phys_start >= virt_off) phys_start -= virt_off;
        if (phys_end   >= virt_off) phys_end   -= virt_off;
        s_proto.mem_map_count = probe_memory(s_mem_map, ZXFL_MEM_MAP_MAX,
                                             phys_start, phys_end, mem_limit);
    }
    s_proto.mem_map_addr = (uint64_t) (uintptr_t) s_mem_map;
    s_proto.mem_total_bytes = sum_usable_ram(s_mem_map, s_proto.mem_map_count);
    s_proto.flags |= ZXFL_FLAG_MEM_MAP;
    s_proto.lowcore_phys = 0;
    s_proto.flags |= ZXFL_FLAG_LOWCORE;

    print("zxfl01: detecting system (stsi/smp/tod)\n");
    zxfl_system_detect(&s_proto);

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
