// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/panic.c

#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/panic.h>

[[noreturn]] void panic(const char *msg) {
    print("*** STOP: ");
    print(msg);
    diag_flush_all();
    arch_sys_halt();
    __builtin_unreachable();
}
