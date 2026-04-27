#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/panic.h

#include <arch/s390x/trap/trap.h>

// ---------------------------------------------------------------------------
// panic_halt - enter a disabled-wait state immediately.
//
// Writes a disabled-wait PSW with address CONFIG_PANIC_HALT_ADDR so the halt
// is visually distinct from a normal end-of-kernel wait in QEMU logs.
// Does not print anything; safe to call before the console is up.
// ---------------------------------------------------------------------------
[[noreturn]] void panic_halt(void);

// ---------------------------------------------------------------------------
// panic - print a formatted message and halt.
// ---------------------------------------------------------------------------
[[noreturn]] void panic(const char *fmt, ...);

// ---------------------------------------------------------------------------
// panic_with_regs - print a register dump, then panic.
// ---------------------------------------------------------------------------
[[noreturn]] void panic_with_regs(const pt_regs_t *regs, const char *fmt, ...);
