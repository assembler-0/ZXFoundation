// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/sclp.c

#include <arch/s390x/cpu/sclp.h>
#include <arch/s390x/cpu/processor.h>
#include <lib/string.h>

/// @brief SCLP instruction wrapper
/// @param sccb Physical address of the SCCB
/// @param command Command code
/// @return CC (0 = success, 1 = busy, 3 = not operational)
static inline int __sclp_servc(uint64_t sccb, uint32_t command) {
    int cc;
    __asm__ volatile (
        "   .insn   rre,0xb2200000,%1,%2\n" // SERVC r1, r2
        "   ipm     %0\n"
        "   srl     %0,28\n"
        : "=d" (cc)
        : "d" (command), "a" (sccb)
        : "cc", "memory"
    );
    return cc;
}

int arch_sclp_service_call(uint32_t command, void *sccb) {
    uint64_t sccb_phys = (uint64_t)sccb;

    if (sccb_phys & 0xFFF)
        return -1;

    // Execute call, retry if busy
    int cc;
    int retry = SCLP_SERVC_MAX_RETRIES;
    do {
        cc = __sclp_servc(sccb_phys, command);
        if (cc == 1) { // Busy
            for (int i = 0; i < SCLP_SERVC_BUSY_DELAY; i++)
                arch_cpu_relax();
        }
    } while (cc == 1 && --retry > 0);

    if (cc != 0)
        return cc;

    struct sccb_header *h = (struct sccb_header *)sccb;
    if (h->response_code != SCLP_RC_NORMAL_COMPLETION)
        return -2;

    return 0;
}

int arch_sclp_early_get_memsize(uint64_t *mem_size_out) {
    static struct read_info_sccb sccb __attribute__((aligned(4096)));
    memset(&sccb, 0, sizeof(sccb));
    sccb.header.length = sizeof(sccb);

    int rc = arch_sclp_service_call(SCLP_CMDW_READ_SCP_INFO, &sccb);
    if (rc != 0)
        return rc;

    uint64_t rnsize = sccb.rnsize ? sccb.rnsize : (sccb.rnmax >> 16);
    if (rnsize == 0) return -3;
    
    // rnsize is in MB
    *mem_size_out = sccb.rnmax * rnsize * 1024UL * 1024UL;
    
    return 0;
}
