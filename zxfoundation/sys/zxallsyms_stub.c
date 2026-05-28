// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/zxallsyms_stub.c
//
/// @brief First-pass link stub for the zxallsyms table.
///
///        During the first link pass the real zxallsyms_data.c has not yet
///        been generated.  This stub provides empty arrays so the binary
///        links cleanly and can be passed to llvm-nm to extract addresses.
///        The second link pass replaces this stub with the generated data.

#include <zxfoundation/types.h>

const uint64_t zxallsyms_addresses[1] = { 0 };
const uint32_t zxallsyms_offsets[1]   = { 0 };
const char     zxallsyms_names[1]     = { '\0' };
const uint32_t zxallsyms_num_syms     = 0;
