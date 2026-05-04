// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/stfle.h
//
/// @brief Expanded STFLE (Store Facility List Extended) API.
#ifndef ZXFOUNDATION_ZXFL_STFLE_H
#define ZXFOUNDATION_ZXFL_STFLE_H

#include <zxfoundation/types.h>

/// @brief Maximum doublewords to request from STFLE.
///        256 facilities = 4 dwords was the old limit.
///        Current z/Architecture supports up to 2048 facilities = 32 dwords.
#define STFLE_MAX_DWORDS    32U

#define STFLE_BIT_ZARCH         2U      ///< z/Architecture installed
#define STFLE_BIT_STFLE         7U      ///< STFLE itself
#define STFLE_BIT_EDAT1         8U      ///< Enhanced-DAT facility 1 (1 MB FC=1 segment entries)
#define STFLE_BIT_EDAT2         78U     ///< Enhanced-DAT facility 2 (2 GB FC=1 region-third entries)
#define STFLE_BIT_EIMM          21U     ///< Extended-immediate facility
#define STFLE_BIT_GEN_INST      25U     ///< General-instructions-extension
#define STFLE_BIT_EXECUTE_EXT   35U     ///< Execute-extensions facility
#define STFLE_BIT_FLOATING_PT   41U     ///< Floating-point extension
#define STFLE_BIT_DFP           42U     ///< Decimal-floating-point
#define STFLE_BIT_PFPO          44U     ///< PFPO instruction
#define STFLE_BIT_VECTOR        129U    ///< Vector facility
#define STFLE_BIT_VECTOR_ENH    135U    ///< Vector-enhancements facility 1
#define STFLE_BIT_VECTOR_ENH2   148U    ///< Vector-enhancements facility 2
#define STFLE_BIT_GUARDED_STOR  133U    ///< Guarded-storage facility
#define STFLE_BIT_MISC_INST_EXT 58U     ///< Miscellaneous-instruction-extensions 1
#define STFLE_BIT_MISC_INST_EX2 76U     ///< Miscellaneous-instruction-extensions 2
#define STFLE_BIT_MISC_INST_EX3 61U     ///< Miscellaneous-instruction-extensions 3

/// @brief Detect CPU facilities using STFLE.
///
///        Stores up to @p max_dwords doublewords into @p fac_list.
///        If STFLE is not available (CC=3), stores zero and returns 0.
///
/// @param fac_list   Output buffer; must hold at least @p max_dwords uint64_t.
/// @param max_dwords Maximum doublewords to request (capped at STFLE_MAX_DWORDS).
/// @return Actual number of doublewords stored, or 0 if STFLE unavailable.
uint32_t stfle_detect(uint64_t *fac_list, uint32_t max_dwords);

/// @brief Test whether a specific facility bit is set.
///
///        Uses big-endian bit numbering: bit 0 is the MSB of fac_list[0].
///        This matches the z/Architecture PoP facility bit numbering.
///
/// @param fac_list  Facility list filled by stfle_detect().
/// @param bit       Facility bit number (0-based, per PoP Appendix B).
/// @return true if the facility is present, false otherwise.
static inline bool stfle_has_facility(const uint64_t *fac_list, uint32_t bit) {
    const uint32_t dword_idx = bit / 64U;
    const uint32_t bit_pos   = 63U - (bit % 64U);  // big-endian bit within dword
    if (dword_idx >= STFLE_MAX_DWORDS)
        return false;
    return (fac_list[dword_idx] >> bit_pos) & 1U;
}

#endif /* ZXFOUNDATION_ZXFL_STFLE_H */
