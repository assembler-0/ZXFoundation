// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/stfle.c

#include <arch/s390x/init/zxfl/stfle.h>

uint32_t stfle_detect(uint64_t *fac_list, uint32_t max_dwords) {
    if (!fac_list || max_dwords == 0)
        return 0;

    if (max_dwords > STFLE_MAX_DWORDS)
        max_dwords = STFLE_MAX_DWORDS;

    for (uint32_t i = 0; i < max_dwords; i++)
        fac_list[i] = 0;

    // STFLE D2(B2): stores facility doublewords at the address in B2+D2.
    // R0 (input)  = max_dwords - 1  (number of dwords to store, minus 1)
    // R0 (output) = actual_dwords - 1
    // The facility list address is passed via the "a" (address) register
    // constraint and referenced as 0(%[addr]) in the instruction operand.
    // This matches the encoding used by Linux arch/s390/kernel/setup.c and
    // s390-tools zipl. Using the Q constraint with r0 already bound to the
    // count causes GCC to encode 0(%r0) as the operand — wrong base register.
    register uint64_t r0   __asm__("0") = (uint64_t)(max_dwords - 1U);
    register uint64_t addr __asm__("1") = (uint64_t)fac_list;
    int cc;

    __asm__ volatile (
        "   .insn   s,0xb2b00000,0(%[addr])\n"
        "   ipm     %[cc]\n"
        "   srl     %[cc],28\n"
        : [cc] "=d" (cc), "+d" (r0)
        : [addr] "a" (addr)
        : "cc", "memory"
    );

    if (cc == 3)
        return 0;

    return (uint32_t)(r0 + 1U);
}
