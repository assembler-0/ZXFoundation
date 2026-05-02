// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/panic.c

#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

static const uint64_t panic_psw[2] __attribute__((aligned(8))) = {
    0x0002000180000000ULL,  // 64-bit disabled-wait
    0x00000000DEAD0000ULL,  // operator-visible halt address
};

[[noreturn]] void panic(const char *msg) {
    print("*** STOP: ");
    print(msg);
    diag_flush_all();
    __asm__ volatile ("lpswe %0\n" :: "Q" (panic_psw) : "memory");
    __builtin_unreachable();
}
