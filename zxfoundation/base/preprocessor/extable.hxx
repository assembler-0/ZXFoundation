/// SPDX-License-Identifier: Apache-2.0
/// @file extable.hxx
/// @brief Exception table support
/// @note requires __zxmacro_want_extable=1

#pragma once

/// @name Exception table
/// @{
#define EX_TABLE(fault_label, fixup_label, type, data)                       \
    ".pushsection .ex_table, \"a\", @progbits\n"                             \
    ".balign 4\n"                                                            \
    ".long   (" stringify(fault_label) ") - .\n"                             \
    ".long   (" stringify(fixup_label) ") - .\n"                             \
    ".short  " stringify(type) "\n"                                          \
    ".short  " stringify(data) "\n"                                          \
    ".popsection\n"

#define EX_TYPE_FIXUP        0
#define EX_TYPE_UA_LOAD_REG  1
#define EX_TYPE_UA_LOAD_RP   2
#define EX_TYPE_UA_STORE     3
#define EX_TYPE_UA_MVCOS_TO  4
#define EX_TYPE_UA_MVCOS_FROM 5
/// @}