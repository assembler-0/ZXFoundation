// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/stfle.h

#ifndef ZXFOUNDATION_ZXFL_STFLE_H
#define ZXFOUNDATION_ZXFL_STFLE_H

#include <zxfoundation/types.h>

#define STFLE_MAX_DWORDS 32

/// @brief Detect CPU facilities using STFLE instruction
/// @param fac_list Pointer to facility list buffer (must hold STFLE_MAX_DWORDS)
/// @return Number of doublewords stored, or 0 if STFLE not available
uint32_t stfle_detect(const uint64_t *fac_list);

#endif /* ZXFOUNDATION_ZXFL_STFLE_H */
