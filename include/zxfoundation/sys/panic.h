#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/epanic.h

#include <arch/s390x/trap/trap.h>

[[noreturn]] void panic_halt(void);
[[noreturn]] void panic(const char *fmt, ...);
[[noreturn]] void panic_with_regs(const pt_regs_t *regs, const char *fmt, ...);
