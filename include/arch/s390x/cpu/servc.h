#pragma once

// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/servc.h - SERVC (Service Call) instruction wrapper

#include <zxfoundation/types.h>

#define SERVC_CC_OK               0   // Command accepted and completed
#define SERVC_CC_BUSY             2   // Interface busy, retry later
#define SERVC_CC_NOT_OPERATIONAL  3   // SCLP not available on this machine

#define SERVC_MAX_RETRIES         3
#define SERVC_BUSY_DELAY          100000  // Spin iterations between retries

// ---------------------------------------------------------------------------
// servc_issue - emit a single SERVC instruction.
//
//   cmd      - SCLP command word
//   sccb_ptr - 4KB-aligned address of the SCCB
//
// Returns the raw condition code (SERVC_CC_*).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// servc_retry - issue SERVC with automatic busy-retry.
//
// Retries up to SERVC_MAX_RETRIES times when CC=2 (busy), spinning for
// SERVC_BUSY_DELAY iterations between attempts.
// Returns the final condition code.
// ---------------------------------------------------------------------------
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
