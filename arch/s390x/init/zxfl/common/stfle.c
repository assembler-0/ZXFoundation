// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/stfle.c

#include <arch/s390x/init/zxfl/stfle.h>

uint32_t stfle_detect(const uint64_t *fac_list) {
    register uint64_t r0 __asm__("0") = STFLE_MAX_DWORDS - 1;
    register uint64_t r1 __asm__("1") = (uint64_t)fac_list;
    int cc;

    __asm__ volatile (
        "   .insn   s,0xb2b00000,%[fac]\n"
        "   ipm     %[cc]\n"
        "   srl     %[cc],28\n"
        : [cc] "=d" (cc), "+d" (r0)
        : [fac] "Q" (*fac_list), "d" (r1)
        : "cc", "memory"
    );

    return (uint32_t)(r0 + 1);
}
