/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/preprocessor/asm.hxx
/// @brief asm macros
/// @note requires __zxmacro_want_asm=1

#pragma once

/// @name Handy macros for setting machine features
/// @{
#define __asm_machine_begin() \
    ".machine push\n"
#define __asm_machine(x) \
    ".machine " stringify(x) "\n"
#define __asm_machine_end() \
    ".machine pop\n"

#define __machine__(mac)      \
    __asm__ (                 \
        __asm_machine_begin() \
        __asm_machine(mac)    \
    )
/// @}

/// @name Handy macros for getting (shifted) CC
/// @{
#define __asm_capture_cc()     \
    "ipm  %[__cc__]\n"         \
    "srl  %[__cc__],28\n"
#define __asm_cc_out(c)        \
    [__cc__] "=d" (c)
/// @}