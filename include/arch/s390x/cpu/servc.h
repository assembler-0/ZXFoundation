#pragma once

// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/servc.h - SERVC (Service Call) instruction wrapper

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>

#define SERVC_CC_OK               0   // Command accepted and completed
#define SERVC_CC_BUSY             2   // Interface busy, retry later
#define SERVC_CC_NOT_OPERATIONAL  3   // SCLP not available on this machine

#define SERVC_MAX_RETRIES         CONFIG_SCLP_SERVC_MAX_RETRIES
#define SERVC_BUSY_DELAY          CONFIG_SCLP_SERVC_BUSY_DELAY

// Perform service call. Return 0 on success, non-zero otherwise. (s390-tools/zipl/boot/sclp.c)
static int servc_issue(unsigned int command, void *sccb) {
    int cc;
    __asm__ volatile(
        "       .insn   rre,0xb2200000,%1,%2\n"
        "       ipm     %0\n"
        "       srl     %0,28"
        : "=&d" (cc)
        : "d" (command), "a" (sccb)
        : "cc", "memory");
    if (cc == 3)
        return 3;
    if (cc == 2)
        return 2;
    return 0;
}

static inline int servc_retry(uint32_t cmd, void *sccb_ptr) {
    int cc;
    for (int attempt = 0; attempt <= SERVC_MAX_RETRIES; ++attempt) {
        cc = servc_issue(cmd, sccb_ptr);
        if (cc != SERVC_CC_BUSY)
            break;
        // DIAG 0x44 yields to the hypervisor, allowing Hercules/z/VM to
        // process the previous SCLP command and clear the busy state.
        // A NOP spin never gives the host CPU a chance to run the SCLP
        // thread, so the interface stays busy indefinitely.
        __asm__ volatile("diag 0,0,0x44" ::: "memory");
    }
    return cc;
}
