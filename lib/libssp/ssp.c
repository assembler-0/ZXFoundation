// SPDX-License-Identifier: Apache-2.0
// lib/ssp.c — Stack protector library

#include <zxfoundation/types.h>
#include <zxfoundation/sys/syschk.h>

#define STACK_CANARY_VALUE 0xDEADBEEFCAFEBABE

uint64_t __stack_chk_guard = STACK_CANARY_VALUE;

[[noreturn]] void __stack_chk_fail(void) {
    zx_system_check(ZX_SYSCHK_MEM_CORRUPT, "ssp: stack protector check fail");
}