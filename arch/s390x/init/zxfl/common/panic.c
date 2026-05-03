// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/panic.c

#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

static constexpr uint64_t panic_psw[2] __attribute__((aligned(8))) = {
    0x0000800180000000ULL,  // 64-bit disabled-wait
    0x0000000000000000ULL,  // halt at 0x0 (always mapped)
};

[[noreturn]] void panic(const char *msg) {
    print("*** STOP: ");
    print(msg);
    diag_flush_all();
    __asm__ volatile ("lpswe %0\n" :: "Q" (panic_psw) : "memory");
    __builtin_unreachable();
}
