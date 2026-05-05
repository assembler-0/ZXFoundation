// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/lowcore.c

#include <arch/s390x/init/zxfl/lowcore.h>

static inline void write_psw(uint64_t offset, uint64_t mask, uint64_t addr) {
    // Direct store to physical lowcore (DAT is off when this is called).
    // The "memory" clobber ensures the compiler does not reorder these
    // stores relative to the subsequent SSM or LPSWE.
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
    // New PSWs for the five maskable interrupt classes.
    // Each points to a distinct halt address so the operator can identify
    // which interrupt fired from the Hercules/z/VM PSW display.
    // Addresses are in the identity-mapped low 2 GB — always valid.
    write_psw(0x080, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0001UL); // External
    write_psw(0x090, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0002UL); // SVC
    write_psw(0x0A0, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0003UL); // Program check
    write_psw(0x0B0, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0004UL); // Machine check
    write_psw(0x0C0, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0005UL); // I/O

    // Restart new PSW at 0x1A0.
    // If an AP issues SIGP Restart to the BSP during the loader window,
    // it must not jump to whatever the channel subsystem left here at IPL.
    // Point it to the same disabled-wait as the other slots.
    write_psw(0x1A0, ZXFL_PSW_MASK_64BIT_DISABLED, 0xDEAD0006UL); // Restart
}
