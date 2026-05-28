// SPDX-License-Identifier: Apache-2.0
// include/lib/libubsanrt/ubsan.h — UBSan runtime ABI types and handler declarations

#pragma once

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// Annotation macro
// ---------------------------------------------------------------------------

/// @brief Applied to every UBSan handler.  Disables UBSan instrumentation
///        on the handler body to prevent recursive invocation.
#define UBSAN_HANDLER __attribute__((no_sanitize("undefined")))

// ---------------------------------------------------------------------------
// Type kind constants (compiler-rt ABI)
// ---------------------------------------------------------------------------

#define UBSAN_TYPE_KIND_INT     0x0000u  ///< Integer type
#define UBSAN_TYPE_KIND_FLOAT   0x0001u  ///< Floating-point type
#define UBSAN_TYPE_KIND_UNKNOWN 0xFFFFu  ///< Unknown / non-scalar type

// ---------------------------------------------------------------------------
// Core ABI structures
// ---------------------------------------------------------------------------

/// @brief Source location embedded in every compiler-generated check struct.
///        Points into .rodata; never heap-allocated.
typedef struct {
    const char *filename;  ///< Source file path (NUL-terminated, .rodata)
    uint32_t    line;      ///< 1-based line number
    uint32_t    column;    ///< 1-based column number
} ubsan_source_location_t;

/// @brief Type descriptor embedded in check structs that carry type info.
///        The flexible array member `name` holds the human-readable type
///        name as a NUL-terminated string.  Do not sizeof() this struct.
typedef struct {
    uint16_t kind;   ///< UBSAN_TYPE_KIND_*
    uint16_t info;   ///< For INT: bits[15:1]=log2(width), bit[0]=is_signed
    char     name[]; ///< Flexible array — NUL-terminated type name
} ubsan_type_descriptor_t;

// ---------------------------------------------------------------------------
// Check data structures
// ---------------------------------------------------------------------------

/// @brief Arithmetic overflow (add, sub, mul, negate, divrem, integer div/0).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_overflow_data_t;

/// @brief Shift-out-of-bounds.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *lhs_type;
    ubsan_type_descriptor_t *rhs_type;
} ubsan_shift_out_of_bounds_data_t;

/// @brief Array out-of-bounds.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *array_type;
    ubsan_type_descriptor_t *index_type;
} ubsan_out_of_bounds_data_t;

/// @brief __builtin_unreachable reached.
typedef struct {
    ubsan_source_location_t loc;
} ubsan_unreachable_data_t;

/// @brief Load of invalid (e.g. out-of-range enum) value.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_invalid_value_data_t;

/// @brief Type mismatch — legacy v0 (Clang < 6).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    unsigned char            alignment;   ///< Required alignment in bytes
    unsigned char            check_kind;  ///< 0=load,1=store,2=ref,3=member,...
} ubsan_type_mismatch_data_t;

/// @brief Type mismatch — v1 (Clang >= 6, current).
///        alignment stored as log2 to fit in a byte.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    uint8_t                  align_log2;       ///< log2(required alignment)
    uint8_t                  type_check_kind;  ///< same encoding as v0 check_kind
} ubsan_type_mismatch_v1_data_t;

/// @brief Nonnull argument violation.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_source_location_t  attr_loc;  ///< Location of the nonnull attribute
    int                      arg_index; ///< 1-based argument index
} ubsan_nonnull_arg_data_t;

/// @brief Nonnull / nullability return violation.
typedef struct {
    ubsan_source_location_t loc;
} ubsan_nonnull_return_data_t;

/// @brief Pointer overflow.
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
    uint8_t                  kind;  ///< 0=trunc, 1=unsigned-sign-change, 2=signed-trunc
} ubsan_implicit_conversion_data_t;

/// @brief CFI indirect-call / virtual-dispatch check failure.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    uint8_t                  check_kind;
} ubsan_cfi_check_fail_data_t;

/// @brief CFI bad-type (vptr sanitizer).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    bool                     available_vtable;
    uint8_t                  type_check_kind;
} ubsan_cfi_bad_type_data_t;

/// @brief VLA bound not positive.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_vla_bound_data_t;

/// @brief Float-to-integer cast overflow.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *from_type;
    ubsan_type_descriptor_t *to_type;
} ubsan_float_cast_overflow_data_t;

/// @brief Indirect function call through incompatible type.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_function_type_mismatch_data_t;

/// @brief Invalid argument to a builtin (e.g. ctz(0), clz(0)).
typedef struct {
    ubsan_source_location_t loc;
    uint8_t                 kind;  ///< 0=ctz, 1=clz
} ubsan_invalid_builtin_data_t;

/// @brief Floating-point division by zero.
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
} ubsan_float_divide_by_zero_data_t;

/// @brief Dynamic type cache miss (vptr sanitizer, C++ only).
typedef struct {
    ubsan_source_location_t  loc;
    ubsan_type_descriptor_t *type;
    const char              *type_check_kind;
} ubsan_dynamic_type_cache_miss_data_t;

// ---------------------------------------------------------------------------
// Handler declarations — Arithmetic
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow_abort(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow_abort(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow_abort(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow(
    ubsan_overflow_data_t *data, unsigned long old_val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow_abort(
    ubsan_overflow_data_t *data, unsigned long old_val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow_abort(
    ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero(
    ubsan_overflow_data_t *data, unsigned long lhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero_abort(
    ubsan_overflow_data_t *data, unsigned long lhs);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_divide_by_zero(
    ubsan_float_divide_by_zero_data_t *data, unsigned long val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_divide_by_zero_abort(
    ubsan_float_divide_by_zero_data_t *data, unsigned long val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow(
    ubsan_pointer_overflow_data_t *data, unsigned long base, unsigned long result);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow_abort(
    ubsan_pointer_overflow_data_t *data, unsigned long base, unsigned long result);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds(
    ubsan_shift_out_of_bounds_data_t *data, unsigned long lhs, unsigned long rhs);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds_abort(
    ubsan_shift_out_of_bounds_data_t *data, unsigned long lhs, unsigned long rhs);

// ---------------------------------------------------------------------------
// Handler declarations — Memory / Type
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch(
    ubsan_type_mismatch_data_t *data, unsigned long ptr);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_abort(
    ubsan_type_mismatch_data_t *data, unsigned long ptr);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1(
    ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1_abort(
    ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds(
    ubsan_out_of_bounds_data_t *data, unsigned long index);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds_abort(
    ubsan_out_of_bounds_data_t *data, unsigned long index);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption(
    ubsan_alignment_assumption_data_t *data,
    unsigned long ptr, unsigned long align, unsigned long offset);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption_abort(
    ubsan_alignment_assumption_data_t *data,
    unsigned long ptr, unsigned long align, unsigned long offset);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_object_size_mismatch(
    ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_object_size_mismatch_abort(
    ubsan_type_mismatch_v1_data_t *data, unsigned long ptr);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_dynamic_type_cache_miss(
    ubsan_dynamic_type_cache_miss_data_t *data,
    unsigned long ptr, unsigned long hash);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_dynamic_type_cache_miss_abort(
    ubsan_dynamic_type_cache_miss_data_t *data,
    unsigned long ptr, unsigned long hash);

// ---------------------------------------------------------------------------
// Handler declarations — Null / Nonnull
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg(
    ubsan_nonnull_arg_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg_abort(
    ubsan_nonnull_arg_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return(
    ubsan_nonnull_return_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_abort(
    ubsan_nonnull_return_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1(
    ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1_abort(
    ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_arg(
    ubsan_nonnull_arg_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_arg_abort(
    ubsan_nonnull_arg_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_return_v1(
    ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_return_v1_abort(
    ubsan_nonnull_return_data_t *data, ubsan_source_location_t *return_loc);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_returns_nonnull_attribute(
    ubsan_nonnull_return_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_returns_nonnull_attribute_abort(
    ubsan_nonnull_return_data_t *data);

// ---------------------------------------------------------------------------
// Handler declarations — Control Flow
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_builtin_unreachable(
    ubsan_unreachable_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_missing_return(
    ubsan_missing_return_data_t *data);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch(
    ubsan_function_type_mismatch_data_t *data, unsigned long val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch_abort(
    ubsan_function_type_mismatch_data_t *data, unsigned long val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail(
    ubsan_cfi_check_fail_data_t *data, unsigned long vtable, unsigned long diag);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail_abort(
    ubsan_cfi_check_fail_data_t *data, unsigned long vtable, unsigned long diag);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_bad_type(
    ubsan_cfi_bad_type_data_t *data, unsigned long vtable,
    bool valid_vtable, unsigned long diag);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_bad_type_abort(
    ubsan_cfi_bad_type_data_t *data, unsigned long vtable,
    bool valid_vtable, unsigned long diag);

// ---------------------------------------------------------------------------
// Handler declarations — Implicit / Misc
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive(
    ubsan_vla_bound_data_t *data, unsigned long bound);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive_abort(
    ubsan_vla_bound_data_t *data, unsigned long bound);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value(
    ubsan_invalid_value_data_t *data, unsigned long val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value_abort(
    ubsan_invalid_value_data_t *data, unsigned long val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion(
    ubsan_implicit_conversion_data_t *data, unsigned long src, unsigned long dst);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion_abort(
    ubsan_implicit_conversion_data_t *data, unsigned long src, unsigned long dst);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow(
    ubsan_float_cast_overflow_data_t *data, unsigned long from_val);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow_abort(
    ubsan_float_cast_overflow_data_t *data, unsigned long from_val);

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin(
    ubsan_invalid_builtin_data_t *data);
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin_abort(
    ubsan_invalid_builtin_data_t *data);

// ---------------------------------------------------------------------------
// ABI hooks
// ---------------------------------------------------------------------------

/// @brief Called by the UBSan runtime to retrieve option string.
///        Returns "" — no runtime options are supported in kernel context.
UBSAN_HANDLER const char *__ubsan_default_options(void);