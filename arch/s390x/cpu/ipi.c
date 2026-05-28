// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/ipi.c

#include <arch/s390x/cpu/ipi.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>

/// @brief Global broadcast message state.
static volatile ipi_msg_t g_broadcast_msg;

void arch_ipi_init(void) {
    arch_ctl_set_bit(0, 14);
}

void arch_ipi_broadcast_wait(ipi_msg_t msg) {
    const uint16_t my_addr = arch_cpu_addr();
    uint32_t target_count = 0;

    g_broadcast_msg = msg;
    barrier();

    // 1. Mark all targets as pending.
    for (unsigned int i = 0; i < MAX_CPUS; i++) {
        zx_lowcore_t *lc = zx_lowcore_cpu(i);
        if (!lc || lc->percpu.cpu_addr == my_addr)
            continue;

        __atomic_store_n(&lc->percpu.ipi_pending_count, 1, __ATOMIC_SEQ_CST);
        target_count++;
    }

    if (target_count == 0)
        return;

    // 2. Send SIGP Emergency Signal to all targets.
    for (unsigned int i = 0; i < MAX_CPUS; i++) {
        zx_lowcore_t *lc = zx_lowcore_cpu(i);
        if (!lc || lc->percpu.cpu_addr == my_addr)
            continue;
        
        sigp_busy(lc->percpu.cpu_addr, SIGP_EMERGENCY_SIGNAL, 0, nullptr);
    }

    for (unsigned int i = 0; i < MAX_CPUS; i++) {
        zx_lowcore_t *lc = zx_lowcore_cpu(i);
        if (!lc || lc->percpu.cpu_addr == my_addr)
            continue;
        
        while (__atomic_load_n(&lc->percpu.ipi_pending_count, __ATOMIC_SEQ_CST) > 0) {
            arch_cpu_relax();
        }
    }
}

void arch_ipi_handle_emergency(void) {
    const ipi_msg_t msg = g_broadcast_msg;

    switch (msg) {
        case IPI_DRAIN_PCP:
            for (int z = 0; z < ZONE_MAX; z++) {
                pmm_drain_local_pcps();
            }
            break;
        
        case IPI_HALT:
            arch_sys_halt();

        default:
            break;
    }

    // Acknowledge completion.
    __atomic_store_n(&zx_lowcore()->percpu.ipi_pending_count, 0, __ATOMIC_SEQ_CST);
}
