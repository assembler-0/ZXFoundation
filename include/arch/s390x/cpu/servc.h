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

static inline int servc_issue(uint32_t cmd, void *sccb_ptr) {
    int cc;
    __asm__ volatile(
        "   .insn   rre,0xb2200000,%1,%2\n"
        "   ipm     %0\n"
        "   srl     %0,28\n"
        : "=&d"(cc)
        : "d"(cmd), "a"(sccb_ptr)
        : "cc", "memory"
    );
    return cc;
}

static inline int servc_retry(uint32_t cmd, void *sccb_ptr) {
    int cc;
    for (int attempt = 0; attempt <= SERVC_MAX_RETRIES; ++attempt) {
        cc = servc_issue(cmd, sccb_ptr);
        if (cc != SERVC_CC_BUSY)
            break;
        for (volatile int i = 0; i < SERVC_BUSY_DELAY; ++i) {}
    }
    return cc;
}
