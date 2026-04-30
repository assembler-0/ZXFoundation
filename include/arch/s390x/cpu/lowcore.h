#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/lowcore.h - s390x Low-Core (prefix area) layout.

#include <zxfoundation/types.h>

typedef struct {
    uint64_t mask;
    uint64_t addr;
} __attribute__((packed)) psw_t;

#define LC_RESTART_OLD_PSW          0x008   // Restart old PSW        (16 bytes)
#define LC_EXT_OLD_PSW              0x018   // External old PSW       (16 bytes)
#define LC_SVC_OLD_PSW              0x028   // Supervisor-call old PSW(16 bytes)
#define LC_PGM_OLD_PSW              0x038   // Program old PSW        (16 bytes)
#define LC_MCHK_OLD_PSW             0x048   // Machine-check old PSW  (16 bytes)
#define LC_IO_OLD_PSW               0x0F8   // I/O old PSW            (16 bytes)

#define LC_EXT_NEW_PSW              0x058   // External new PSW       (16 bytes)
#define LC_SVC_NEW_PSW              0x068   // Supervisor-call new PSW(16 bytes)
#define LC_PGM_NEW_PSW              0x078   // Program new PSW        (16 bytes)
#define LC_MCHK_NEW_PSW             0x088   // Machine-check new PSW  (16 bytes)
#define LC_IO_NEW_PSW               0x1C8   // I/O new PSW            (16 bytes)
#define LC_RESTART_NEW_PSW          0x1A0   // Restart new PSW        (16 bytes)

#define LC_PGM_ILC                  0x08C   // Program interrupt ILC (2 bytes)
#define LC_PGM_CODE                 0x08E   // Program interrupt code (2 bytes)
#define LC_TRANS_EXC_ADDR           0x090   // Translation exception address (8 bytes)
#define LC_EXT_INT_CODE             0x086   // External interrupt code (2 bytes)
#define LC_SVC_INT_CODE             0x088   // SVC interrupt code (2 bytes)

typedef struct {
    uint8_t  _pad000[0x008];            // 0x000 - 0x007  IPL PSW / reserved

    psw_t    restart_old_psw;           // 0x008 - 0x017
    psw_t    ext_old_psw;               // 0x018 - 0x027
    psw_t    svc_old_psw;               // 0x028 - 0x037
    psw_t    pgm_old_psw;               // 0x038 - 0x047
    psw_t    mchk_old_psw;              // 0x048 - 0x057

    psw_t    ext_new_psw;               // 0x058 - 0x067
    psw_t    svc_new_psw;               // 0x068 - 0x077
    psw_t    pgm_new_psw;               // 0x078 - 0x087
    psw_t    mchk_new_psw;              // 0x088 - 0x097

    uint8_t  _pad098[0x004];            // 0x098 - 0x09B  reserved
    uint8_t  _pad09c[0x002];            // 0x09C - 0x09D  reserved
    uint8_t  _pad09e[0x002];            // 0x09E - 0x09F  reserved

    uint8_t  _pad0a0[0x058];            // 0x0A0 - 0x0F7  reserved

    psw_t    io_old_psw;                // 0x0F8 - 0x107

    uint8_t  _pad108[0x098];            // 0x108 - 0x19F  reserved

    psw_t    restart_new_psw;           // 0x1A0 - 0x1AF

    uint8_t  _pad1b0[0x018];            // 0x1B0 - 0x1C7  reserved

    psw_t    io_new_psw;                // 0x1C8 - 0x1D7
} __attribute__((packed)) lowcore_t;

static inline volatile lowcore_t *lowcore(void) {
    return (volatile lowcore_t *)0UL;
}
