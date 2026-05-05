// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/system.c
//
/// @brief System detection: SMP CPUs, STSI branding, TOD clock.

#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/cpu/stsi.h>
#include <arch/s390x/init/zxfl/ebcdic.h>
#include <arch/s390x/init/zxfl/string.h>

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
    uint16_t bsp;
    arch_cpu_addr(bsp);
    
    proto->bsp_cpu_addr = bsp;
    proto->cpu_count = 0;
    
    // Probe up to 64 potential CPU addresses.
    // In z/Architecture, CPU addresses can be sparse, but for simplicity
    // in this environment, scanning 0..ZXFL_CPU_MAP_MAX-1 is sufficient.
    for (uint16_t addr = 0; addr < ZXFL_CPU_MAP_MAX; addr++) {
        if (addr == bsp) {
            proto->cpu_map[proto->cpu_count].cpu_addr = addr;
            proto->cpu_map[proto->cpu_count].type = ZXFL_CPU_TYPE_UNKNOWN;
            proto->cpu_map[proto->cpu_count].state = ZXFL_CPU_ONLINE;
            proto->cpu_count++;
            continue;
        }
        
        int cc = sigp_sense(addr);
        if (cc == 3) { // Not operational
            continue;
        }
        
        // CC 0, 1, or 2 means the CPU exists
        proto->cpu_map[proto->cpu_count].cpu_addr = addr;
        proto->cpu_map[proto->cpu_count].type = ZXFL_CPU_TYPE_UNKNOWN;
        proto->cpu_map[proto->cpu_count].state = ZXFL_CPU_STOPPED;
        proto->cpu_count++;
    }
    
    if (proto->cpu_count > 0) {
        proto->flags |= ZXFL_FLAG_SMP;
    }
}

static void detect_tod(zxfl_boot_protocol_t *proto) {
    uint64_t tod;
    __asm__ volatile ("stck %0" : "=Q" (tod) : : "cc", "memory");
    proto->tod_boot = tod;
    proto->flags |= ZXFL_FLAG_TOD;
}

void zxfl_system_detect(zxfl_boot_protocol_t *proto) {
    memset(&proto->sysinfo, 0, sizeof(proto->sysinfo));
    detect_stsi(proto);
    detect_smp(proto);
    detect_tod(proto);
}
