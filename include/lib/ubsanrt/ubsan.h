// SPDX-License-Identifier: Apache-2.0
// include/lib/ubsanrt/ubsan.h — UBSAN runtime ABI types and handler declarations

#pragma once

#include <zxfoundation/types.h>

/// @brief Applied to every UBSAN handler definition and declaration.
#define UBSAN_HANDLER __attribute__((no_sanitize("undefined")))

#define UBSAN_TYPE_KIND_INT     0u  ///< Integer type
#define UBSAN_TYPE_KIND_FLOAT   1u  ///< Floating-point type
#define UBSAN_TYPE_KIND_UNKNOWN 0xFFFFu

/// @brief Source location embedded in every compiler-generated check struct.
typedef struct {
    const char *filename;   ///< Source file path (NUL-terminated, .rodata)
    uint32_t    line;       ///< 1-based line number
    uint32_t    column;     ///< 1-based column number
} ubsan_source_location_t;

/// @brief Type descriptor embedded in check structs that carry type info.
///
///        For integer types:
///          kind = UBSAN_TYPE_KIND_INT
///          info: bit 0 = 1 if signed, bits 15:1 = (bit_width - 1)
///        For float types:
///          kind = UBSAN_TYPE_KIND_FLOAT
///          info: bit_width
///        name[]: flexible array, NUL-terminated C type name string
typedef struct {
    uint16_t kind;
    uint16_t info;
    char     name[];    ///< Flexible array — do not sizeof() this struct
} ubsan_type_descriptor_t;

/// @brief Arithmetic overflow (add, sub, mul, negate, divrem).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_overflow_data_t;

/// @brief Shift out of bounds.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *lhs_type;
    ubsan_type_descriptor_t *rhs_type;
} ubsan_shift_out_of_bounds_data_t;

/// @brief Array index out of bounds.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *array_type;
    ubsan_type_descriptor_t *index_type;
} ubsan_out_of_bounds_data_t;

/// @brief __builtin_unreachable() reached.
typedef struct {
    ubsan_source_location_t loc;
} ubsan_unreachable_data_t;

/// @brief Load of invalid value (enum, bool).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_invalid_value_data_t;

/// @brief Type mismatch v1 (null deref, misaligned access, wrong dynamic type).
///        The v1 variant encodes alignment as log2 rather than the raw value.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    uint8_t                  align_log2; ///< required alignment = 1 << align_log2
    uint8_t                  type_check_kind;
} ubsan_type_mismatch_v1_data_t;

/// @brief Non-null argument violation.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_source_location_t  attr_loc;  ///< location of the nonnull attribute
    int                      arg_index;
} ubsan_nonnull_arg_data_t;

/// @brief Non-null return value violation.
typedef struct {
    ubsan_source_location_t loc;
} ubsan_nonnull_return_data_t;

/// @brief Pointer arithmetic overflow.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_pointer_overflow_data_t;

/// @brief Missing return from non-void function.
typedef struct {
    ubsan_source_location_t loc;
} ubsan_missing_return_data_t;

/// @brief Alignment assumption violation (__builtin_assume_aligned).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_source_location_t  assumption_loc;
    ubsan_type_descriptor_t *type;
} ubsan_alignment_assumption_data_t;

/// @brief Implicit integer conversion (truncation, sign change).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *from_type;
    ubsan_type_descriptor_t *to_type;
    uint8_t                  kind;  ///< 0=truncation, 1=sign-change, 2=both
} ubsan_implicit_conversion_data_t;

/// @brief CFI check failure.
typedef struct {
    ubsan_source_location_t loc;
    ubsan_type_descriptor_t *type;
    uint8_t                  check_kind;
} ubsan_cfi_check_fail_data_t;

/// @brief VLA bound not positive.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_vla_bound_data_t;

/// @brief Float cast overflow.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *from_type;
    ubsan_type_descriptor_t *to_type;
} ubsan_float_cast_overflow_data_t;

/// @brief Function type mismatch (indirect call through wrong pointer type).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_function_type_mismatch_data_t;

/// @brief Invalid builtin argument (e.g., __builtin_ctz(0)).
typedef struct {
    ubsan_source_location_t loc;
    uint8_t                 kind;
} ubsan_invalid_builtin_data_t;

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow_abort(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow_abort(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow_abort(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow(ubsan_overflow_data_t *data, unsigned long old_val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow_abort(ubsan_overflow_data_t *data, unsigned long old_val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow_abort(ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds(ubsan_shift_out_of_bounds_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds_abort(ubsan_shift_out_of_bounds_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds(ubsan_out_of_bounds_data_t *data, unsigned long index);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds_abort(ubsan_out_of_bounds_data_t *data, unsigned long index);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_builtin_unreachable(ubsan_unreachable_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value(ubsan_invalid_value_data_t *data, unsigned long val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value_abort(ubsan_invalid_value_data_t *data, unsigned long val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1(ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1_abort(ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg(ubsan_nonnull_arg_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg_abort(ubsan_nonnull_arg_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return(ubsan_nonnull_return_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1(ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1_abort(ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow(ubsan_pointer_overflow_data_t *data, unsigned long base, unsigned long result);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow_abort(ubsan_pointer_overflow_data_t *data, unsigned long base, unsigned long result);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_missing_return(ubsan_missing_return_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption(ubsan_alignment_assumption_data_t *data, unsigned long ptr, unsigned long align, unsigned long offset);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption_abort(ubsan_alignment_assumption_data_t *data, unsigned long ptr, unsigned long align, unsigned long offset);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion(ubsan_implicit_conversion_data_t *data, unsigned long src, unsigned long dst);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion_abort(ubsan_implicit_conversion_data_t *data, unsigned long src, unsigned long dst);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail(ubsan_cfi_check_fail_data_t *data, unsigned long vtable, unsigned long diag);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail_abort(ubsan_cfi_check_fail_data_t *data, unsigned long vtable, unsigned long diag);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive(ubsan_vla_bound_data_t *data, unsigned long bound);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive_abort(ubsan_vla_bound_data_t *data, unsigned long bound);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow(ubsan_float_cast_overflow_data_t *data, unsigned long from_val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow_abort(ubsan_float_cast_overflow_data_t *data, unsigned long from_val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch(ubsan_function_type_mismatch_data_t *data, unsigned long val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch_abort(ubsan_function_type_mismatch_data_t *data, unsigned long val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin(ubsan_invalid_builtin_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin_abort(ubsan_invalid_builtin_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero(ubsan_overflow_data_t *data, unsigned long lhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero_abort(ubsan_overflow_data_t *data, unsigned long lhs);
