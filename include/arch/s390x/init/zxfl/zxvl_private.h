// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxvl_private.h

#ifndef ZXFOUNDATION_ZXVL_PRIVATE_H
#define ZXFOUNDATION_ZXVL_PRIVATE_H

#include <zxfoundation/types.h>

#define ZXVL_SEED               UINT64_C(0xA5F0C3E1B2D49687)

#define ZXVL_COMPUTE_TOKEN(stfle0, schid) \
    (ZXVL_SEED ^ (uint64_t)(stfle0) ^ (uint64_t)(schid))

#define ZXVL_HS_OFFSET          0x0UL
#define ZXVL_HS_CHALLENGE       UINT64_C(0xFEEDFACECAFEBABE)
#define ZXVL_HS_RESPONSE        UINT64_C(0xDEADBEEF0BADF00D)

#define ZXVL_FRAME_MAGIC_A      UINT64_C(0xC0FFEE00FACADE42)
#define ZXVL_FRAME_MAGIC_B      UINT64_C(0x1337BABE0DDBA115)
#define ZXVL_FRAME_SIZE         64U


#define ZXVL_LOCK_MASK          UINT64_C(0x3C1E0F8704B2D596)
#define ZXVL_LOCK_EXPECTED      UINT64_C(0xF0A5C3B2E1D49687)
#define ZXVL_LOCK_EMBED_HI      0xCCBBCC35U
#define ZXVL_LOCK_EMBED_LO      0xE5664311U
#define ZXVL_LOCK_OFFSET        0x70000U
#define ZXVL_LOCK_GAP           0x1000U
#define ZXVL_LOCK_SENTINEL      0x5A58464CU

#define _ZX_CH2(s, i)   ((uint32_t)((s)[i] - '0') * 10U + (uint32_t)((s)[i+1] - '0'))
#define ZXVL_BUILD_TS   ((_ZX_CH2(__TIME__, 0) << 24) | \
                         (_ZX_CH2(__TIME__, 3) << 16) | \
                         (_ZX_CH2(__TIME__, 6) <<  8) | \
                          0x5AU)

#endif /* ZXFOUNDATION_ZXVL_PRIVATE_H */
