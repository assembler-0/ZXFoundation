// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/lowcore.c

#include <arch/s390x/init/zxfl/lowcore.h>

#define HALT_ADDR_EXT       0x0000000000DEAD10ULL
#define HALT_ADDR_SVC       0x0000000000DEAD20ULL
#define HALT_ADDR_PGM       0x0000000000DEAD30ULL
#define HALT_ADDR_MCK       0x0000000000DEAD40ULL
#define HALT_ADDR_IO        0x0000000000DEAD50ULL

void zxfl_lowcore_setup(void) {
    volatile zxfl_lowcore_t *lc = (volatile zxfl_lowcore_t *)0x0;
    
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
