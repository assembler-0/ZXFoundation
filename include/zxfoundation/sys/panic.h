#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/epanic.h

[[noreturn]] void panic_halt(void);
[[noreturn]] void panic(const char *fmt, ...);