// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/lowcore.c

#include <arch/s390x/init/zxfl/lowcore.h>
#include <zxfoundation/zconfig.h>

// Halt addresses are HHDM virtual: the CPU will fetch them with DAT on.
// Using hhdm_phys_to_virt() keeps them in the identity-mapped HHDM window
// so the disabled-wait PSW is always reachable regardless of kernel state.
// Disabled-wait PSWs have DAT=0 — addresses are physical and purely diagnostic.
// Use small recognizable values so the operator can identify which interrupt
// caused the halt from the PSW address in the Hercules console.
#define HALT_ADDR_EXT   0x0000000000DEAD10ULL
#define HALT_ADDR_SVC   0x0000000000DEAD20ULL
#define HALT_ADDR_PGM   0x0000000000DEAD30ULL
#define HALT_ADDR_MCK   0x0000000000DEAD40ULL
#define HALT_ADDR_IO    0x0000000000DEAD50ULL

void zxfl_lowcore_setup(void) {
    // Lowcore lives at physical 0x0; access via HHDM since DAT is on.
    volatile zxfl_lowcore_t *lc =
        (volatile zxfl_lowcore_t *)hhdm_phys_to_virt(0);

    lc->ext_new_psw.mask = ZXFL_PSW_MASK_64BIT_DISABLED;
    lc->ext_new_psw.addr = HALT_ADDR_EXT;

    lc->svc_new_psw.mask = ZXFL_PSW_MASK_64BIT_DISABLED;
    lc->svc_new_psw.addr = HALT_ADDR_SVC;

    lc->pgm_new_psw.mask = ZXFL_PSW_MASK_64BIT_DISABLED;
    lc->pgm_new_psw.addr = HALT_ADDR_PGM;

    lc->mck_new_psw.mask = ZXFL_PSW_MASK_64BIT_DISABLED;
    lc->mck_new_psw.addr = HALT_ADDR_MCK;

    lc->io_new_psw.mask = ZXFL_PSW_MASK_64BIT_DISABLED;
    lc->io_new_psw.addr = HALT_ADDR_IO;
}
