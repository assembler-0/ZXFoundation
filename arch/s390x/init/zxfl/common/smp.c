// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/smp.c

#include <arch/s390x/init/zxfl/smp.h>
#include <arch/s390x/cpu/stsi.h>

static struct sysinfo_1_2_2 s_sysinfo __attribute__((aligned(4096)));

static int sigp_issue(uint16_t target_addr, uint8_t order) {
    register uint64_t r1 __asm__("1") = 0;
    register uint64_t r3 __asm__("3") = order;
    int cc;
    __asm__ volatile (
        "   sigp    %[r1],%[addr],%[ord]\n"
        "   ipm     %[cc]\n"
        "   srl     %[cc],28\n"
        : [cc] "=d" (cc), [r1] "+d" (r1)
        : [addr] "d" ((uint64_t)target_addr), [ord] "d" (r3)
        : "cc"
    );
    return cc;
}

uint16_t zxfl_smp_current_cpu_addr(void) {
    uint16_t addr;
    __asm__ volatile ("stap %0" : "=Q" (addr));
    return addr;
}

uint32_t zxfl_smp_enumerate(zxfl_cpu_info_t *buf, uint32_t max) {
    if (!buf || max == 0)
        return 0;

    const uint16_t bsp_addr = zxfl_smp_current_cpu_addr();

    if (stsi(&s_sysinfo, 1, 2, 2) == 0) {
        uint32_t total = s_sysinfo.cpus_configured ? s_sysinfo.cpus_configured : 1;
        buf[0] = (zxfl_cpu_info_t){ bsp_addr, 0xFF, ZXFL_CPU_ONLINE, 0 };
        uint32_t count = 1;
        uint16_t addr = 0;
        for (uint32_t i = 1; i < total && count < max; i++) {
            if (addr == bsp_addr) addr++;
            buf[count++] = (zxfl_cpu_info_t){ addr++, 0xFF, ZXFL_CPU_STOPPED, 0 };
        }
        return count;
    }

    buf[0] = (zxfl_cpu_info_t){ bsp_addr, 0xFF, ZXFL_CPU_ONLINE, 0 };
    return 1;
}

void zxfl_smp_stop_aps(const zxfl_cpu_info_t *cpu_map,
                       uint32_t count,
                       uint16_t bsp_addr) {
    for (uint32_t i = 0; i < count; i++) {
        if (cpu_map[i].cpu_addr == bsp_addr)
            continue;
        for (uint32_t retry = 0; retry < SIGP_MAX_RETRIES; retry++) {
            if (sigp_issue(cpu_map[i].cpu_addr, SIGP_STOP) != SIGP_CC_BUSY)
                break;
            __asm__ volatile ("diag 0,0,0x44" ::: "memory");
        }
    }
}
