// SPDX-License-Identifier: Apache-2.0
// lib/libubsanrt/ubsan.c — Undefined Behavior Sanitizer runtime for ZXFoundation

#include <lib/ubsanrt/ubsan.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <lib/vsprintf.h>

static volatile bool g_ubsan_active;

/// @brief Decode the type descriptor into a human-readable classification string.
UBSAN_HANDLER static const char *ubsan_type_kind_str(const ubsan_type_descriptor_t *td) {
    static char buf[48];
    if (!td)
        return "<null-type>";
    switch (td->kind) {
        case UBSAN_TYPE_KIND_INT: {
            unsigned int bit_width = (unsigned int)(td->info >> 1u) + 1u;
            const char  *sign     = (td->info & 1u) ? "signed" : "unsigned";
            snprintf(buf, sizeof(buf), "%s %u-bit integer", sign, bit_width);
            return buf;
        }
        case UBSAN_TYPE_KIND_FLOAT: {
            unsigned int bit_width = (unsigned int)td->info;
            snprintf(buf, sizeof(buf), "%u-bit float", bit_width);
            return buf;
        }
        default:
            return "unknown type";
    }
}

/// @brief Format a source location into buf.  Safe if loc is nullptr.
UBSAN_HANDLER static void ubsan_fmt_loc(char *buf, size_t sz,
                                        const ubsan_source_location_t *loc) {
    if (!loc || !loc->filename)
        snprintf(buf, sz, "<unknown>:0:0");
    else
        snprintf(buf, sz, "%s:%u:%u", loc->filename, loc->line, loc->column);
}

/// @brief Central report-and-halt path.
///
///        Attempts to emit a printk diagnostic.  If g_ubsan_active is already
///        set (recursive invocation), skips printk and halts immediately.
///        The syschk call is unconditional — it never returns.
[[noreturn]] UBSAN_HANDLER static void ubsan_report_and_halt(
    const ubsan_source_location_t *loc,
    const char *violation,
    const char *detail) {
    if (!__atomic_test_and_set(&g_ubsan_active, __ATOMIC_SEQ_CST)) {
        char loc_buf[96];
        ubsan_fmt_loc(loc_buf, sizeof(loc_buf), loc);
        printk(ZX_ERROR "ubsan: %s at %s\n", violation, loc_buf);
        if (detail[0])
            printk(ZX_ERROR "ubsan: detail: %s\n", detail);
    }

    zx_system_check(ZX_SYSCHK_CORE_CORRUPT,
                    "ubsan: undefined behavior: %s", violation);
}

// ---------------------------------------------------------------------------
// Overflow handlers
//
// The compiler emits calls to these for signed/unsigned arithmetic overflow.
// Both the non-abort and _abort variants are identical in our implementation
// because we always halt.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow(ubsan_overflow_data_t *data,
                                               unsigned long lhs, unsigned long rhs) {
    char detail[128];
    const char *tname = data ? ubsan_type_kind_str(data->type) : "?";
    snprintf(detail, sizeof(detail),
             "%s addition overflow: lhs=0x%lx rhs=0x%lx", tname, lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "add-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow_abort(ubsan_overflow_data_t *data,
                                                     unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_add_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow(ubsan_overflow_data_t *data,
                                               unsigned long lhs, unsigned long rhs) {
    char detail[128];
    const char *tname = data ? ubsan_type_kind_str(data->type) : "?";
    snprintf(detail, sizeof(detail),
             "%s subtraction overflow: lhs=0x%lx rhs=0x%lx", tname, lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "sub-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow_abort(ubsan_overflow_data_t *data,
                                                     unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_sub_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow(ubsan_overflow_data_t *data,
                                               unsigned long lhs, unsigned long rhs) {
    char detail[128];
    const char *tname = data ? ubsan_type_kind_str(data->type) : "?";
    snprintf(detail, sizeof(detail),
             "%s multiplication overflow: lhs=0x%lx rhs=0x%lx", tname, lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "mul-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow_abort(ubsan_overflow_data_t *data,
                                                     unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_mul_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow(ubsan_overflow_data_t *data,
                                                  unsigned long old_val) {
    char detail[96];
    const char *tname = data ? ubsan_type_kind_str(data->type) : "?";
    snprintf(detail, sizeof(detail),
             "%s negation overflow: val=0x%lx", tname, old_val);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "negate-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow_abort(ubsan_overflow_data_t *data,
                                                        unsigned long old_val) {
    __ubsan_handle_negate_overflow(data, old_val);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow(ubsan_overflow_data_t *data,
                                                  unsigned long lhs, unsigned long rhs) {
    char detail[128];
    const char *tname = data ? ubsan_type_kind_str(data->type) : "?";
    if (rhs == 0)
        snprintf(detail, sizeof(detail),
                 "%s division by zero: lhs=0x%lx", tname, lhs);
    else
        snprintf(detail, sizeof(detail),
                 "%s divrem overflow: lhs=0x%lx rhs=0x%lx", tname, lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "divrem-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow_abort(ubsan_overflow_data_t *data,
                                                        unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_divrem_overflow(data, lhs, rhs);
}

// ---------------------------------------------------------------------------
// Shift out-of-bounds
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds(
    ubsan_shift_out_of_bounds_data_t *data,
    unsigned long lhs, unsigned long rhs) {
    char detail[160];
    const char *lname = (data && data->lhs_type)
                            ? ubsan_type_kind_str(data->lhs_type)
                            : "?";
    const char *rname = (data && data->rhs_type)
                            ? ubsan_type_kind_str(data->rhs_type)
                            : "?";
    snprintf(detail, sizeof(detail),
             "shift of %s (0x%lx) by %s (0x%lx) out of bounds",
             lname, lhs, rname, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "shift-out-of-bounds", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds_abort(
    ubsan_shift_out_of_bounds_data_t *data,
    unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_shift_out_of_bounds(data, lhs, rhs);
}

// ---------------------------------------------------------------------------
// Array out-of-bounds
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds(ubsan_out_of_bounds_data_t *data,
                                                unsigned long index) {
    char detail[128];
    const char *aname = (data && data->array_type)
                            ? ubsan_type_kind_str(data->array_type)
                            : "?";
    const char *iname = (data && data->index_type)
                            ? ubsan_type_kind_str(data->index_type)
                            : "?";
    snprintf(detail, sizeof(detail),
             "index 0x%lx (%s) out of bounds for %s",
             index, iname, aname);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "out-of-bounds", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds_abort(ubsan_out_of_bounds_data_t *data,
                                                      unsigned long index) {
    __ubsan_handle_out_of_bounds(data, index);
}

// ---------------------------------------------------------------------------
// Unreachable
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_builtin_unreachable(ubsan_unreachable_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "builtin-unreachable",
                          "execution reached __builtin_unreachable()");
}

// ---------------------------------------------------------------------------
// Invalid value (load of invalid enum/bool)
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value(ubsan_invalid_value_data_t *data,
                                                     unsigned long val) {
    char detail[128];
    const char *tname = (data && data->type)
                            ? ubsan_type_kind_str(data->type)
                            : "?";
    snprintf(detail, sizeof(detail),
             "load of invalid value 0x%lx for type %s", val, tname);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "load-invalid-value", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value_abort(ubsan_invalid_value_data_t *data,
                                                           unsigned long val) {
    __ubsan_handle_load_invalid_value(data, val);
}

// ---------------------------------------------------------------------------
// Type mismatch (null pointer dereference, misaligned access, wrong type)
//
// The v1 variant encodes alignment as log2 in data->align_log2.
// align = 0 means no alignment requirement (pointer type check only).
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1(ubsan_type_mismatch_v1_data_t *data,
                                                   unsigned long ptr) {
    char detail[192];

    if (!data) {
        ubsan_report_and_halt(nullptr, "type-mismatch", "null data pointer");
    }

    if (ptr == 0UL) {
        snprintf(detail, sizeof(detail),
                 "null pointer dereference (type: %s)",
                 data->type ? ubsan_type_kind_str(data->type) : "?");
        ubsan_report_and_halt(&data->loc, "null-pointer-dereference", detail);
    }

    if (data->align_log2 > 0u) {
        unsigned long align = 1UL << data->align_log2;
        if (ptr & (align - 1UL)) {
            snprintf(detail, sizeof(detail),
                     "misaligned access: ptr=0x%lx required_align=%lu (type: %s)",
                     ptr, align,
                     data->type ? ubsan_type_kind_str(data->type) : "?");
            ubsan_report_and_halt(&data->loc, "misaligned-access", detail);
        }
    }

    // Object size mismatch (pointer to wrong type).
    snprintf(detail, sizeof(detail),
             "type mismatch: ptr=0x%lx (type: %s)",
             ptr, data->type ? ubsan_type_kind_str(data->type) : "?");
    ubsan_report_and_halt(&data->loc, "type-mismatch", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1_abort(ubsan_type_mismatch_v1_data_t *data,
                                                         unsigned long ptr) {
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

// ---------------------------------------------------------------------------
// Non-null argument violations
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg(ubsan_nonnull_arg_data_t *data) {
    char detail[64];
    if (data && data->arg_index >= 0)
        snprintf(detail, sizeof(detail), "null passed to argument %d (nonnull)", data->arg_index + 1);
    else
        snprintf(detail, sizeof(detail), "null pointer passed to nonnull parameter");
    ubsan_report_and_halt(data ? &data->loc : nullptr, "nonnull-arg", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg_abort(ubsan_nonnull_arg_data_t *data) {
    __ubsan_handle_nonnull_arg(data);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return(
        ubsan_nonnull_return_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "nonnull-return",
                          "null pointer returned from nonnull function");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    const ubsan_source_location_t *loc =
        (return_loc && return_loc->filename) ? return_loc
        : (data ? &data->loc : nullptr);
    ubsan_report_and_halt(loc,
                          "nonnull-return",
                          "null pointer returned from nonnull function");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1_abort(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    __ubsan_handle_nonnull_return_v1(data, return_loc);
}


// ---------------------------------------------------------------------------
// Pointer overflow
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow(ubsan_pointer_overflow_data_t *data,
                                                   unsigned long base, unsigned long result) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "pointer overflow: base=0x%lx result=0x%lx", base, result);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "pointer-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow_abort(ubsan_pointer_overflow_data_t *data,
                                                         unsigned long base,
                                                         unsigned long result) {
    __ubsan_handle_pointer_overflow(data, base, result);
}

// ---------------------------------------------------------------------------
// Missing return
//
// Fires when a non-void function falls off the end without returning.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_missing_return(ubsan_missing_return_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "missing-return",
                          "execution reached end of non-void function");
}

// ---------------------------------------------------------------------------
// Alignment assumption violation
//
// Fires when __builtin_assume_aligned() or [[assume(aligned)]] is violated.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption(
    ubsan_alignment_assumption_data_t *data,
    unsigned long ptr, unsigned long align, unsigned long offset) {
    char detail[160];
    snprintf(detail, sizeof(detail),
             "alignment assumption violated: ptr=0x%lx align=%lu offset=%lu",
             ptr, align, offset);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "alignment-assumption", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption_abort(
    ubsan_alignment_assumption_data_t *data,
    unsigned long ptr, unsigned long align, unsigned long offset) {
    __ubsan_handle_alignment_assumption(data, ptr, align, offset);
}

// ---------------------------------------------------------------------------
// Implicit conversion (integer truncation / sign change)
//
// Fires when an implicit conversion loses information (e.g., int → char
// truncation, or unsigned → signed with value out of range).
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion(
    ubsan_implicit_conversion_data_t *data,
    unsigned long src, unsigned long dst) {
    char detail[192];
    const char *from = (data && data->from_type)
                           ? ubsan_type_kind_str(data->from_type)
                           : "?";
    const char *to = (data && data->to_type)
                         ? ubsan_type_kind_str(data->to_type)
                         : "?";
    snprintf(detail, sizeof(detail),
             "implicit conversion from %s (0x%lx) to %s (0x%lx) loses information",
             from, src, to, dst);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "implicit-conversion", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion_abort(
    ubsan_implicit_conversion_data_t *data,
    unsigned long src, unsigned long dst) {
    __ubsan_handle_implicit_conversion(data, src, dst);
}

// ---------------------------------------------------------------------------
// CFI check failure
//
// Fires when Control Flow Integrity detects an indirect call to an invalid
// target.  Only relevant when -fsanitize=cfi is also active.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail(ubsan_cfi_check_fail_data_t *data,
                                                 unsigned long vtable, unsigned long diag) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "CFI check failed: vtable=0x%lx diag=0x%lx", vtable, diag);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "cfi-check-fail", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail_abort(ubsan_cfi_check_fail_data_t *data,
                                                       unsigned long vtable,
                                                       unsigned long diag) {
    __ubsan_handle_cfi_check_fail(data, vtable, diag);
}

// ---------------------------------------------------------------------------
// VLA bound not positive
//
// Fires when a variable-length array is declared with a non-positive bound.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive(
    ubsan_vla_bound_data_t *data, unsigned long bound) {
    char detail[96];
    snprintf(detail, sizeof(detail),
             "VLA bound not positive: bound=0x%lx", bound);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "vla-bound-not-positive", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive_abort(
    ubsan_vla_bound_data_t *data, unsigned long bound) {
    __ubsan_handle_vla_bound_not_positive(data, bound);
}

// ---------------------------------------------------------------------------
// Float cast overflow
//
// Fires when a floating-point value is cast to an integer type and the value
// is out of range for the target type.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow(
    ubsan_float_cast_overflow_data_t *data, unsigned long from_val) {
    char detail[160];
    const char *from = (data && data->from_type)
                           ? ubsan_type_kind_str(data->from_type)
                           : "?";
    const char *to = (data && data->to_type)
                         ? ubsan_type_kind_str(data->to_type)
                         : "?";
    snprintf(detail, sizeof(detail),
             "float cast overflow: value 0x%lx (%s) out of range for %s",
             from_val, from, to);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "float-cast-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow_abort(
    ubsan_float_cast_overflow_data_t *data, unsigned long from_val) {
    __ubsan_handle_float_cast_overflow(data, from_val);
}

// ---------------------------------------------------------------------------
// Function type mismatch
//
// Fires when an indirect function call is made through a pointer whose
// declared type does not match the actual function's type.
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch(
    ubsan_function_type_mismatch_data_t *data, unsigned long val) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "function type mismatch: callee=0x%lx", val);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "function-type-mismatch", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch_abort(
    ubsan_function_type_mismatch_data_t *data, unsigned long val) {
    __ubsan_handle_function_type_mismatch(data, val);
}

// ---------------------------------------------------------------------------
// Invalid builtin
//
// Fires when a builtin is called with an invalid argument (e.g., __builtin_ctz(0)).
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin(ubsan_invalid_builtin_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "invalid-builtin",
                          "invalid argument to builtin function");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin_abort(ubsan_invalid_builtin_data_t *data) {
    __ubsan_handle_invalid_builtin(data);
}

// ---------------------------------------------------------------------------
// Integer truncation (GCC-specific variant)
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero(ubsan_overflow_data_t *data,
                                                         unsigned long lhs) {
    char detail[96];
    snprintf(detail, sizeof(detail), "integer divide by zero: lhs=0x%lx", lhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "integer-divide-by-zero", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero_abort(ubsan_overflow_data_t *data,
                                                               unsigned long lhs) {
    __ubsan_handle_integer_divide_by_zero(data, lhs);
}
