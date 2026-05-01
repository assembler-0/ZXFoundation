// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/dasd.h
//
/// @brief Compatibility umbrella header.
///        Includes both dasd_io.h (raw channel I/O) and dasd_vtoc.h
///        (VTOC dataset search) so existing code that includes dasd.h
///        continues to compile without modification.
///
///        New code should include the specific sub-header it needs.

#ifndef ZXFOUNDATION_ZXFL_DASD_H
#define ZXFOUNDATION_ZXFL_DASD_H

#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>

#endif /* ZXFOUNDATION_ZXFL_DASD_H */
