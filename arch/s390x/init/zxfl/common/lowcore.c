// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/lowcore.c

#include <arch/s390x/init/zxfl/lowcore.h>

static inline void write_psw(uint64_t offset, uint64_t mask, uint64_t addr) {
    __asm__ volatile (
        "stg %[mask], 0(%[off])\n"
        "stg %[addr], 8(%[off])\n"
        :
        : [mask] "r" (mask),
          [addr] "r" (addr),
          [off]  "r" (offset)
        : "memory"
    );
}

void zxfl_lowcore_setup(void) {
    write_psw(0x080, ZXFL_PSW_MASK_64BIT_DISABLED, 0x000000000000DEAD10ULL);
    write_psw(0x090, ZXFL_PSW_MASK_64BIT_DISABLED, 0x000000000000DEAD20ULL);
    write_psw(0x0A0, ZXFL_PSW_MASK_64BIT_DISABLED, 0x000000000000DEAD30ULL);
    write_psw(0x0B0, ZXFL_PSW_MASK_64BIT_DISABLED, 0x000000000000DEAD40ULL);
    write_psw(0x0C0, ZXFL_PSW_MASK_64BIT_DISABLED, 0x000000000000DEAD50ULL);
}
