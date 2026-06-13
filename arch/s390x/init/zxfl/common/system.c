// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/system.c
//
/// @brief System detection: SMP CPUs, STSI branding, TOD clock.

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/stsi.h>
#include <arch/s390x/init/zxfl/ebcdic.h>
#include <arch/s390x/init/zxfl/string.h>

/// @brief stap — returns the hardware CPU address (used for SIGP, not as a logical ID).
static inline unsigned short arch_cpu_addr(void) {
    unsigned short cpu_address;
    __asm__ volatile("stap %0" : "=m" (cpu_address));
    return cpu_address;
}

static void copy_ebcdic_field(char *dest, const char *src, uint32_t len) {
    if (len == 0) return;
    memcpy(dest, src, len);
    ebcdic_to_ascii_buf(dest, len);
    uint32_t end = len;
    while (end > 0 && dest[end - 1] == ' ')
        end--;
    dest[end < len ? end : len - 1] = '\0';
}

static void detect_stsi(zxfl_boot_protocol_t *proto) {
    static uint8_t block[4096] __attribute__((aligned(4096)));
    
    // 1.1.1: Manufacturer, Type, Model
    if (stsi(block, 1, 1, 1) == 0) {
        struct sysinfo_1_1_1 *s = (struct sysinfo_1_1_1 *)block;
        copy_ebcdic_field(proto->sysinfo.manufacturer, s->manufacturer, 16);
        copy_ebcdic_field(proto->sysinfo.type, s->type, 4);
        copy_ebcdic_field(proto->sysinfo.model, s->model, 16);
        copy_ebcdic_field(proto->sysinfo.sequence, s->sequence, 16);
        copy_ebcdic_field(proto->sysinfo.plant, s->plant, 4);
        proto->flags |= ZXFL_FLAG_SYSINFO;
    }

    // 1.2.2: CPU configuration
    if (stsi(block, 1, 2, 2) == 0) {
        struct sysinfo_1_2_2 *s = (struct sysinfo_1_2_2 *)block;
        proto->sysinfo.cpus_total = s->cpus_total;
        proto->sysinfo.cpus_configured = s->cpus_configured;
        proto->sysinfo.cpus_standby = s->cpus_standby;
        proto->sysinfo.capability = s->capability;
    }

    // 2.2.2: LPAR info
    if (stsi(block, 2, 2, 2) == 0) {
        struct sysinfo_2_2_2 *s = (struct sysinfo_2_2_2 *)block;
        copy_ebcdic_field(proto->sysinfo.lpar_name, s->name, 8);
        proto->sysinfo.lpar_number = s->lpar_number;
    }
}

static int sigp_sense(uint16_t cpu_addr) {
    register uint32_t reg1 __asm__("1") = 0;
    register uint32_t reg2 __asm__("2") = cpu_addr;
    int cc;
    __asm__ volatile (
        "sigp %1, %2, 1\n" // 1 = Sense
        "ipm  %0\n"
        "srl  %0, 28\n"
        : "=d" (cc), "+d" (reg1)
        : "d" (reg2)
        : "cc", "memory"
    );
    return cc;
}

static void detect_smp(zxfl_boot_protocol_t *proto) {
    uint16_t bsp = arch_cpu_addr();
    
    proto->bsp_cpu_addr = bsp;
    proto->cpu_count = 0;

    static uint8_t stsi_buf[4096] __attribute__((aligned(4096)));
    int has_topology = 0;

    // Query CPU topology for each CPU type.
    // sel1 selects the CPU type: 1=CP, 2=IFL, 3=ICF, 4=zIIP (ZXFL_CPU_TYPE_*).
    // sel2=6 selects the topology-list format (mandatory per PoP).
    for (uint8_t sel1 = 1; sel1 <= 4; sel1++) {
        if (stsi(stsi_buf, 15, sel1, 6) == 0) {
            struct sysinfo_15_1_x *info = (struct sysinfo_15_1_x *)stsi_buf;
            if (info->length >= sizeof(struct sysinfo_15_1_x)) {
                has_topology = 1;

                // Container IDs are updated as the parser descends the TLE tree.
                // All IDs are normalized from PoP 1-based to 0-based by subtracting 1.
                uint8_t drawer_id = 0;
                uint8_t book_id   = 0;
                uint8_t socket_id = 0;

                uint8_t *ptr = (uint8_t *)info->tle;
                uint8_t *end = (uint8_t *)info + info->length;

                while (ptr + 8 <= end && proto->cpu_count < ZXFL_CPU_MAP_MAX) {
                    union topology_entry *tle = (union topology_entry *)ptr;

                    if (tle->nl == 0) {
                        // Core leaf (topology_core, 16 bytes).
                        // mask is MSB-first (IBM bit 0 = MSB of 64-bit word):
                        //   bit k set => CPU at address (origin + k) is present.
                        if (ptr + 16 > end) break;

                        uint64_t mask   = tle->cpu.mask;
                        uint16_t origin = tle->cpu.origin;

                        for (int k = 0; k < 64 && proto->cpu_count < ZXFL_CPU_MAP_MAX; k++) {
                            if (!((mask >> (63 - k)) & 1ULL)) continue;

                            uint16_t addr = origin + (uint16_t)k;

                            // Skip CPUs already registered from a previous sel1 pass.
                            int already_mapped = 0;
                            for (uint32_t i = 0; i < proto->cpu_count; i++) {
                                if (proto->cpu_map[i].cpu_addr == addr) {
                                    already_mapped = 1;
                                    break;
                                }
                            }
                            if (already_mapped) continue;

                            // Verify the CPU responds to SIGP Sense (CC=3 means not operational).
                            if (sigp_sense(addr) == 3) continue;

                            zxfl_cpu_info_t *ci = &proto->cpu_map[proto->cpu_count];
                            ci->cpu_addr  = addr;
                            ci->type      = sel1;
                            ci->state     = (addr == bsp) ? ZXFL_CPU_ONLINE : ZXFL_CPU_STOPPED;
                            ci->drawer_id = drawer_id;
                            ci->book_id   = book_id;
                            ci->socket_id = socket_id;
                            ci->chip_id   = socket_id;
                            ci->thread_id = 0;
                            ci->numa_node = drawer_id;

                            proto->cpu_count++;
                        }
                        ptr += 16;
                    } else {
                        switch (tle->nl) {
                            case 1:
                                socket_id = (tle->container.id > 0U)
                                            ? (uint8_t)(tle->container.id - 1U) : 0U;
                                break;
                            case 2:
                                book_id = (tle->container.id > 0U)
                                          ? (uint8_t)(tle->container.id - 1U) : 0U;
                                break;
                            case 3:
                                drawer_id = (tle->container.id > 0U)
                                            ? (uint8_t)(tle->container.id - 1U) : 0U;
                                break;
                            default:
                                break;
                        }
                        ptr += 8;
                    }
                }
            }
        }
    }

    if (!has_topology || proto->cpu_count == 0) {
        proto->cpu_count = 0;
        for (uint16_t addr = 0; addr < ZXFL_CPU_MAP_MAX; addr++) {
            if (addr == bsp) {
                zxfl_cpu_info_t *ci = &proto->cpu_map[proto->cpu_count];
                ci->cpu_addr  = addr;
                ci->type      = ZXFL_CPU_TYPE_CP;
                ci->state     = ZXFL_CPU_ONLINE;
                ci->drawer_id = 0;
                ci->book_id   = 0;
                ci->socket_id = 0;
                ci->chip_id   = 0;
                ci->thread_id = 0;
                ci->numa_node = 0;
                proto->cpu_count++;
                continue;
            }

            if (sigp_sense(addr) == 3) continue;

            // Assign 2 CPUs per synthetic drawer; drawer_id is the NUMA domain.
            uint8_t drawer = (uint8_t)((proto->cpu_count / 2U) % 4U);

            zxfl_cpu_info_t *ci = &proto->cpu_map[proto->cpu_count];
            ci->cpu_addr  = addr;
            ci->type      = ZXFL_CPU_TYPE_CP;
            ci->state     = ZXFL_CPU_STOPPED;
            ci->drawer_id = drawer;
            ci->book_id   = 0;
            ci->socket_id = 0;
            ci->chip_id   = 0;
            ci->thread_id = 0;
            ci->numa_node = drawer;  // drawer = NUMA affinity domain (see STSI path above)
            proto->cpu_count++;
        }
    }

    if (proto->cpu_count > 0) {
        proto->flags |= ZXFL_FLAG_SMP;
    }
}

uint64_t tod_read(void) {
    uint64_t clk;
    __asm__ volatile("stckf %0" : "=Q"(clk) :: "cc");
    return clk;
}

static void detect_tod(zxfl_boot_protocol_t *proto) {
    proto->tod_boot = tod_read();
    proto->flags |= ZXFL_FLAG_TOD;
}

void zxfl_system_detect(zxfl_boot_protocol_t *proto) {
    memset(&proto->sysinfo, 0, sizeof(proto->sysinfo));
    detect_stsi(proto);
    detect_smp(proto);
    detect_tod(proto);
}
