// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/parmfile.h
//
/// @brief Freestanding parmfile parameter parser for the ZXFL bootloader.
///
///        Parses a raw ASCII command-line string (as loaded from
///        ETC.ZXFOUNDATION.PARM) for the syssize= parameter.
///
///        syssize= syntax:
///            syssize=<decimal_megabytes>[M|G]

#ifndef ZXFOUNDATION_ZXFL_PARMFILE_H
#define ZXFOUNDATION_ZXFL_PARMFILE_H

#include <zxfoundation/types.h>

/// @brief Minimum legal syssize value (16 MB — below this the kernel cannot boot, except 0 which means infinite).
#define PARMFILE_SYSSIZE_MIN_MB     16UL

/// @brief Maximum legal syssize value (512 GB — z/Architecture physical limit).
#define PARMFILE_SYSSIZE_MAX_MB     (512UL * 1024UL)

/// @brief Value representing an infinite / uncapped memory limit.
#define PARMFILE_SYSSIZE_INFINITE   UINT64_MAX

/// @brief Parse the syssize= parameter from a raw ASCII command line.
///
///        Scans the NUL-terminated string @p cmdline for the token
///        "syssize=<value>[M|G]".  The token must be preceded by a
///        space or be at the start of the string.  The value is
///        converted to bytes.
///
///        If the parameter is absent, or if the value is explicitly 0,
///        returns PARMFILE_SYSSIZE_INFINITE (UINT64_MAX).
///
/// @param cmdline  NUL-terminated ASCII command line (from parmfile).
/// @param len      Length of @p cmdline in bytes (excluding NUL).
///                 Parsing stops at @p len even if no NUL is found.
/// @return Physical memory ceiling in bytes, aligned down to 1 MB, or PARMFILE_SYSSIZE_INFINITE.
///         Returns 0 if the parameter is malformed or out of range.
uint64_t parse_syssize(const char *cmdline, uint32_t len);

#endif /* ZXFOUNDATION_ZXFL_PARMFILE_H */
