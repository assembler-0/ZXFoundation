// SPDX-License-Identifier: Apache-2.0
// lib/libubsanrt/ubsan.c — Undefined Behavior Sanitizer runtime for ZXFoundation

#include <lib/libubsanrt/ubsan.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <lib/vsprintf.h>

/// @brief Set by the first CPU to enter a UBSan handler.
///        Prevents duplicate log output on SMP.  Never cleared.
static volatile unsigned char g_ubsan_active = 0;

/// @brief Decode a type descriptor into a human-readable string.
UBSAN_HANDLER static const char *ubsan_type_str(const ubsan_type_descriptor_t *td) {
    static char buf[128];

    if (!td)
        return "<null-type>";

    // The compiler always provides a non-empty name for scalar types.
    if (td->name[0] != '\0')
        return td->name;

    switch (td->kind) {
        case UBSAN_TYPE_KIND_INT: {
            // Clang encoding: bits[15:1] = log2(bit_width), bit[0] = is_signed
            unsigned int bit_width = 1u << (td->info >> 1u);
            const char  *sign      = (td->info & 1u) ? "signed" : "unsigned";
            snprintf(buf, sizeof(buf), "%s %u-bit integer", sign, bit_width);
            return buf;
        }
        case UBSAN_TYPE_KIND_FLOAT: {
            snprintf(buf, sizeof(buf), "%u-bit float", (unsigned int)td->info);
            return buf;
        }
        default:
            return "unknown-type";
    }
}

/// @brief Format a source location into buf.  Safe when loc is nullptr.
UBSAN_HANDLER static void ubsan_fmt_loc(char *buf, size_t sz,
                                        const ubsan_source_location_t *loc) {
    if (!loc || !loc->filename)
        snprintf(buf, sz, "<unknown>:0:0");
    else
        snprintf(buf, sz, "%s:%u:%u", loc->filename, loc->line, loc->column);
}

/// @brief Translate a type_check_kind byte into a descriptive verb phrase.
UBSAN_HANDLER static const char *ubsan_check_kind_str(unsigned char kind) {
    switch (kind) {
        case 0:  return "load of";
        case 1:  return "store to";
        case 2:  return "reference binding to";
        case 3:  return "member access within";
        case 4:  return "member call on";
        case 5:  return "constructor call on";
        case 6:  return "downcast of";
        case 7:  return "downcast of";
        case 8:  return "upcast of";
        case 9:  return "cast from non-virtual base of";
        case 10: return "dynamic operation on";
        default: return "operation on";
    }
}

/// @brief Log the violation and halt the kernel.
///
///        The __atomic_test_and_set ensures only one CPU emits the log.
///        All CPUs reach zx_system_check() regardless — the disabled-wait
///        PSW halts each CPU independently.
[[noreturn]] UBSAN_HANDLER static void ubsan_report_and_halt(
        const ubsan_source_location_t *loc,
        const char                    *violation,
        const char                    *detail) {

    // Only the first CPU to arrive logs; others skip straight to halt.
    if (!__atomic_test_and_set(&g_ubsan_active, __ATOMIC_SEQ_CST)) {
        char loc_buf[96];
        ubsan_fmt_loc(loc_buf, sizeof(loc_buf), loc);
        printk(ZX_ERROR "ubsan: %s at %s\n", violation, loc_buf);
        if (detail && detail[0])
            printk(ZX_ERROR "ubsan: detail: %s\n", detail);
    }

    zx_system_check(ZX_SYSCHK_CORE_CORRUPT,
                    "ubsan: undefined behavior detected: %s", violation);
}

/// @brief No runtime options are supported in kernel context.
UBSAN_HANDLER const char *__ubsan_default_options(void) {
    return "";
}

// ---------------------------------------------------------------------------
// ARITHMETIC HANDLERS
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    char detail[160];
    snprintf(detail, sizeof(detail), "%s addition overflow: 0x%lx + 0x%lx",
             ubsan_type_str(data ? data->type : nullptr), lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "add-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_add_overflow_abort(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_add_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    char detail[160];
    snprintf(detail, sizeof(detail), "%s subtraction overflow: 0x%lx - 0x%lx",
             ubsan_type_str(data ? data->type : nullptr), lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "sub-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_sub_overflow_abort(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_sub_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    char detail[160];
    snprintf(detail, sizeof(detail), "%s multiplication overflow: 0x%lx * 0x%lx",
             ubsan_type_str(data ? data->type : nullptr), lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "mul-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_mul_overflow_abort(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_mul_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow(
        ubsan_overflow_data_t *data, unsigned long old_val) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s negation overflow: -(0x%lx)",
             ubsan_type_str(data ? data->type : nullptr), old_val);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "negate-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_negate_overflow_abort(
        ubsan_overflow_data_t *data, unsigned long old_val) {
    __ubsan_handle_negate_overflow(data, old_val);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    char detail[160];
    if (rhs == 0)
        snprintf(detail, sizeof(detail), "%s division by zero: 0x%lx / 0",
                 ubsan_type_str(data ? data->type : nullptr), lhs);
    else
        snprintf(detail, sizeof(detail), "%s divrem overflow: 0x%lx / 0x%lx",
                 ubsan_type_str(data ? data->type : nullptr), lhs, rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr, "divrem-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_divrem_overflow_abort(
        ubsan_overflow_data_t *data, unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_divrem_overflow(data, lhs, rhs);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero(
        ubsan_overflow_data_t *data, unsigned long lhs) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s integer division by zero: lhs=0x%lx",
             ubsan_type_str(data ? data->type : nullptr), lhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "integer-divide-by-zero", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_integer_divide_by_zero_abort(
        ubsan_overflow_data_t *data, unsigned long lhs) {
    __ubsan_handle_integer_divide_by_zero(data, lhs);
}

/// @brief Floating-point division by zero.
///        val is the raw bit pattern of the dividend passed in an integer
///        register by the compiler (the ABI passes float args as integers
///        in freestanding mode when -msoft-float is active).
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_divide_by_zero(
        ubsan_float_divide_by_zero_data_t *data, unsigned long val) {
    char detail[160];
    snprintf(detail, sizeof(detail),
             "%s floating-point division by zero: dividend bits=0x%lx",
             ubsan_type_str(data ? data->type : nullptr), val);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "float-divide-by-zero", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_divide_by_zero_abort(
        ubsan_float_divide_by_zero_data_t *data, unsigned long val) {
    __ubsan_handle_float_divide_by_zero(data, val);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow(
        ubsan_pointer_overflow_data_t *data,
        unsigned long base, unsigned long result) {
    char detail[160];
    snprintf(detail, sizeof(detail),
             "pointer overflow: base=0x%lx result=0x%lx", base, result);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "pointer-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_pointer_overflow_abort(
        ubsan_pointer_overflow_data_t *data,
        unsigned long base, unsigned long result) {
    __ubsan_handle_pointer_overflow(data, base, result);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds(
        ubsan_shift_out_of_bounds_data_t *data,
        unsigned long lhs, unsigned long rhs) {
    char detail[192];
    snprintf(detail, sizeof(detail),
             "shift of %s (0x%lx) by %s (0x%lx) is out of bounds",
             ubsan_type_str(data ? data->lhs_type : nullptr), lhs,
             ubsan_type_str(data ? data->rhs_type : nullptr), rhs);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "shift-out-of-bounds", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_shift_out_of_bounds_abort(
        ubsan_shift_out_of_bounds_data_t *data,
        unsigned long lhs, unsigned long rhs) {
    __ubsan_handle_shift_out_of_bounds(data, lhs, rhs);
}

// ---------------------------------------------------------------------------
// MEMORY / TYPE MISMATCH HANDLERS
// ---------------------------------------------------------------------------

/// @brief Common implementation for all type-mismatch variants.
///
///        Distinguishes three sub-cases:
///          1. ptr == 0  → null pointer dereference
///          2. ptr misaligned for the type → misaligned access
///          3. otherwise → insufficient object size
///
///        alignment == 0 means "no alignment requirement" (skip check 2).
[[noreturn]] UBSAN_HANDLER static void ubsan_type_mismatch_common(
        const ubsan_source_location_t *loc,
        const ubsan_type_descriptor_t *type,
        unsigned long                  alignment,
        unsigned char                  check_kind,
        unsigned long                  ptr) {
    char detail[256];
    const char *verb = ubsan_check_kind_str(check_kind);
    const char *tname = ubsan_type_str(type);

    if (ptr == 0) {
        snprintf(detail, sizeof(detail),
                 "%s null pointer of type %s", verb, tname);
        ubsan_report_and_halt(loc, "null-pointer-dereference", detail);
    }

    if (alignment && (ptr & (alignment - 1u))) {
        snprintf(detail, sizeof(detail),
                 "%s misaligned address 0x%lx for type %s"
                 " (requires %lu-byte alignment)",
                 verb, ptr, tname, alignment);
        ubsan_report_and_halt(loc, "misaligned-access", detail);
    }

    snprintf(detail, sizeof(detail),
             "%s address 0x%lx with insufficient space for type %s",
             verb, ptr, tname);
    ubsan_report_and_halt(loc, "insufficient-object-size", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch(
        ubsan_type_mismatch_data_t *data, unsigned long ptr) {
    ubsan_type_mismatch_common(
        data ? &data->loc       : nullptr,
        data ? data->type       : nullptr,
        data ? (unsigned long)data->alignment : 0UL,
        data ? data->check_kind : 0,
        ptr);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_abort(
        ubsan_type_mismatch_data_t *data, unsigned long ptr) {
    __ubsan_handle_type_mismatch(data, ptr);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1(
        ubsan_type_mismatch_v1_data_t *data, unsigned long ptr) {
    // align_log2 == 0 means 1-byte alignment (no constraint).
    unsigned long align = data ? (1UL << data->align_log2) : 0UL;
    ubsan_type_mismatch_common(
        data ? &data->loc            : nullptr,
        data ? data->type            : nullptr,
        align,
        data ? data->type_check_kind : 0,
        ptr);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_type_mismatch_v1_abort(
        ubsan_type_mismatch_v1_data_t *data, unsigned long ptr) {
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

/// @brief Object-size mismatch — emitted by -fsanitize=object-size.
///        Uses the same v1 data layout as type_mismatch_v1.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_object_size_mismatch(
        ubsan_type_mismatch_v1_data_t *data, unsigned long ptr) {
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_object_size_mismatch_abort(
        ubsan_type_mismatch_v1_data_t *data, unsigned long ptr) {
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds(
        ubsan_out_of_bounds_data_t *data, unsigned long index) {
    char detail[192];
    snprintf(detail, sizeof(detail),
             "index %lu out of range for type %s",
             index, ubsan_type_str(data ? data->array_type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "out-of-bounds", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_out_of_bounds_abort(
        ubsan_out_of_bounds_data_t *data, unsigned long index) {
    __ubsan_handle_out_of_bounds(data, index);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption(
        ubsan_alignment_assumption_data_t *data,
        unsigned long ptr, unsigned long align, unsigned long offset) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "__builtin_assume_aligned: address 0x%lx (offset %lu)"
             " violates %lu-byte alignment assumption",
             ptr, offset, align);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "alignment-assumption-failed", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_alignment_assumption_abort(
        ubsan_alignment_assumption_data_t *data,
        unsigned long ptr, unsigned long align, unsigned long offset) {
    __ubsan_handle_alignment_assumption(data, ptr, align, offset);
}

/// @brief Dynamic type cache miss — emitted by -fsanitize=vptr (C++ only).
///        hash is the Itanium ABI type hash used for the vtable lookup.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_dynamic_type_cache_miss(
        ubsan_dynamic_type_cache_miss_data_t *data,
        unsigned long ptr, unsigned long hash) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "dynamic type check failed: ptr=0x%lx hash=0x%lx type=%s",
             ptr, hash,
             ubsan_type_str(data ? data->type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "dynamic-type-cache-miss", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_dynamic_type_cache_miss_abort(
        ubsan_dynamic_type_cache_miss_data_t *data,
        unsigned long ptr, unsigned long hash) {
    __ubsan_handle_dynamic_type_cache_miss(data, ptr, hash);
}

// ---------------------------------------------------------------------------
// NULL / NONNULL HANDLERS
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg(
        ubsan_nonnull_arg_data_t *data) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "null pointer passed as argument %d"
             " (declared nonnull)",
             data ? data->arg_index : 0);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "nonnull-arg-violation", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_arg_abort(
        ubsan_nonnull_arg_data_t *data) {
    __ubsan_handle_nonnull_arg(data);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return(
        ubsan_nonnull_return_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "nonnull-return-violation",
                          "null pointer returned from function"
                          " declared to never return null");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_abort(
        ubsan_nonnull_return_data_t *data) {
    __ubsan_handle_nonnull_return(data);
}

/// @brief v1 variant — compiler passes an additional return-site location.
///        Prefer return_loc for the diagnostic; fall back to data->loc.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    const ubsan_source_location_t *loc =
        return_loc ? return_loc : (data ? &data->loc : nullptr);
    ubsan_report_and_halt(loc,
                          "nonnull-return-violation",
                          "null pointer returned from function"
                          " declared to never return null");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nonnull_return_v1_abort(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    __ubsan_handle_nonnull_return_v1(data, return_loc);
}

/// @brief Nullability arg — same semantics as nonnull_arg but for _Nonnull.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_arg(
        ubsan_nonnull_arg_data_t *data) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "null pointer passed as argument %d"
             " (marked _Nonnull)",
             data ? data->arg_index : 0);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "nullability-arg-violation", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_arg_abort(
        ubsan_nonnull_arg_data_t *data) {
    __ubsan_handle_nullability_arg(data);
}

/// @brief Nullability return v1 — same semantics as nonnull_return_v1
///        but for _Nonnull-annotated return types.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_return_v1(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    const ubsan_source_location_t *loc =
        return_loc ? return_loc : (data ? &data->loc : nullptr);
    ubsan_report_and_halt(loc,
                          "nullability-return-violation",
                          "null pointer returned from function"
                          " marked _Nonnull");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_nullability_return_v1_abort(
        ubsan_nonnull_return_data_t *data,
        ubsan_source_location_t     *return_loc) {
    __ubsan_handle_nullability_return_v1(data, return_loc);
}

/// @brief Returns-nonnull-attribute — emitted by some Clang versions as an
///        alias for nonnull_return when the __attribute__((returns_nonnull))
///        annotation is used rather than _Nonnull.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_returns_nonnull_attribute(
        ubsan_nonnull_return_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "returns-nonnull-attribute-violated",
                          "null pointer returned from function"
                          " with __attribute__((returns_nonnull))");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_returns_nonnull_attribute_abort(
        ubsan_nonnull_return_data_t *data) {
    __ubsan_handle_returns_nonnull_attribute(data);
}

// ---------------------------------------------------------------------------
// CONTROL FLOW HANDLERS
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_builtin_unreachable(
        ubsan_unreachable_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "unreachable-reached",
                          "execution reached __builtin_unreachable()");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_missing_return(
        ubsan_missing_return_data_t *data) {
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "missing-return",
                          "execution reached end of non-void function"
                          " without returning a value");
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch(
        ubsan_function_type_mismatch_data_t *data, unsigned long val) {
    char detail[192];
    snprintf(detail, sizeof(detail),
             "call to function at 0x%lx through pointer"
             " with incompatible type %s",
             val, ubsan_type_str(data ? data->type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "function-type-mismatch", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_function_type_mismatch_abort(
        ubsan_function_type_mismatch_data_t *data, unsigned long val) {
    __ubsan_handle_function_type_mismatch(data, val);
}

/// @brief CFI indirect-call / virtual-dispatch check failure.
///        vtable is the address of the vtable pointer read from the object.
///        diag  is an opaque diagnostic cookie from the compiler.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail(
        ubsan_cfi_check_fail_data_t *data,
        unsigned long vtable, unsigned long diag) {
    char detail[192];
    snprintf(detail, sizeof(detail),
             "CFI check failed: vtable=0x%lx diag=0x%lx type=%s",
             vtable, diag,
             ubsan_type_str(data ? data->type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "cfi-check-fail", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_check_fail_abort(
        ubsan_cfi_check_fail_data_t *data,
        unsigned long vtable, unsigned long diag) {
    __ubsan_handle_cfi_check_fail(data, vtable, diag);
}

/// @brief CFI bad-type — vptr sanitizer variant that also reports whether
///        a valid vtable was found at the address.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_bad_type(
        ubsan_cfi_bad_type_data_t *data,
        unsigned long vtable, bool valid_vtable, unsigned long diag) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "CFI bad type: vtable=0x%lx valid_vtable=%s diag=0x%lx type=%s",
             vtable, valid_vtable ? "yes" : "no", diag,
             ubsan_type_str(data ? data->type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "cfi-bad-type", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_cfi_bad_type_abort(
        ubsan_cfi_bad_type_data_t *data,
        unsigned long vtable, bool valid_vtable, unsigned long diag) {
    __ubsan_handle_cfi_bad_type(data, vtable, valid_vtable, diag);
}

// ---------------------------------------------------------------------------
// IMPLICIT CONVERSION / MISC HANDLERS
// ---------------------------------------------------------------------------

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive(
        ubsan_vla_bound_data_t *data, unsigned long bound) {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "variable-length array bound %ld is not positive",
             (long)bound);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "vla-bound-not-positive", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_vla_bound_not_positive_abort(
        ubsan_vla_bound_data_t *data, unsigned long bound) {
    __ubsan_handle_vla_bound_not_positive(data, bound);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value(
        ubsan_invalid_value_data_t *data, unsigned long val) {
    char detail[192];
    snprintf(detail, sizeof(detail),
             "load of value 0x%lx is not a valid value for type %s",
             val, ubsan_type_str(data ? data->type : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "load-invalid-value", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_load_invalid_value_abort(
        ubsan_invalid_value_data_t *data, unsigned long val) {
    __ubsan_handle_load_invalid_value(data, val);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion(
        ubsan_implicit_conversion_data_t *data,
        unsigned long src, unsigned long dst) {
    char detail[256];
    const char *reason = "unknown";
    switch (data ? data->kind : 0xFFu) {
        case 0: reason = "integer truncation";          break;
        case 1: reason = "unsigned integer sign change"; break;
        case 2: reason = "signed integer truncation";   break;
    }
    snprintf(detail, sizeof(detail),
             "implicit conversion from %s to %s"
             " changed value (0x%lx -> 0x%lx) [%s]",
             ubsan_type_str(data ? data->from_type : nullptr),
             ubsan_type_str(data ? data->to_type   : nullptr),
             src, dst, reason);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "implicit-conversion", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_implicit_conversion_abort(
        ubsan_implicit_conversion_data_t *data,
        unsigned long src, unsigned long dst) {
    __ubsan_handle_implicit_conversion(data, src, dst);
}

/// @brief Float-to-integer cast overflow.
///        from_val is the raw bit pattern of the source float value,
///        passed as an integer register by the compiler.
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow(
        ubsan_float_cast_overflow_data_t *data, unsigned long from_val) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "float cast overflow: value with bits 0x%lx (type %s)"
             " is outside the range of type %s",
             from_val,
             ubsan_type_str(data ? data->from_type : nullptr),
             ubsan_type_str(data ? data->to_type   : nullptr));
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "float-cast-overflow", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_float_cast_overflow_abort(
        ubsan_float_cast_overflow_data_t *data, unsigned long from_val) {
    __ubsan_handle_float_cast_overflow(data, from_val);
}

/// @brief Invalid argument to a builtin function.
///        kind 0 = ctz(0), kind 1 = clz(0).
[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin(
        ubsan_invalid_builtin_data_t *data) {
    char detail[128];
    const char *which = "unknown-builtin";
    switch (data ? data->kind : 0xFFu) {
        case 0: which = "ctz(0)"; break;
        case 1: which = "clz(0)"; break;
    }
    snprintf(detail, sizeof(detail),
             "passing zero to %s, which is undefined", which);
    ubsan_report_and_halt(data ? &data->loc : nullptr,
                          "invalid-builtin-arg", detail);
}

[[noreturn]] UBSAN_HANDLER void __ubsan_handle_invalid_builtin_abort(
        ubsan_invalid_builtin_data_t *data) {
    __ubsan_handle_invalid_builtin(data);
}
