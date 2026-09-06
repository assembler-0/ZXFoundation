/// SPDX-License-Identifier: Apache-2.0
/// @file bits.hxx
/// @brief bits/bytes utility
/// @note requires __zxmacro_want_bits=1

#pragma once

/// @name bitstruct/struct support
/// @deprecated manual bitfields are deprecated in favor of @c lib.bitfield and @c lib.structure
/// @{
#define bitsfield(size, name) ubit_int<size> name : (size)
#define bitfield(name) bitsfield(1, name)
#define bitlimit(expr, bits) expr : bits
#define bitrsvd(size) bitsfield(size, concat(__bitrsvd_,__COUNTER__))
#define bytersvd(size) u8 concat(__bytersvd_,__COUNTER__)[size]
/// @}

/// @name bytes/bits conversion
/// @note these definitions are for System/360 and later
/// @{
#define __b_TO_BYTES(bytes) (bytes)
#define __hw_TO_BYTES(hwords) __b_TO_BYTES((hwords) * 2)
#define __w_TO_BYTES(words)   __hw_TO_BYTES((words) * 2)
#define __dw_TO_BYTES(dwords) __w_TO_BYTES((dwords) * 2)
#define __qw_TO_BYTES(qwords) __dw_TO_BYTES((qwords) * 2)
#define tobytes(type, val) concat(concat(__,type),_TO_BYTES)(val)

#define __b_BITS 7
#define __hw_BITS 15
#define __w_BITS  31
#define __dw_BITS 63
#define ibmbit(type, x) (concat(concat(__,type),_BITS) - (x))
#define ibmbitmask(type, x) (1ULL << (concat(concat(__,type),_BITS) - (x)))
#define ibmbitmask8(x)  ibmbitmask(b, x)
#define ibmbitmask16(x) ibmbitmask(hw, x)
#define ibmbitmask32(x) ibmbitmask(w, x)
#define ibmbitmask64(x) ibmbitmask(dw, x)
/// @}
