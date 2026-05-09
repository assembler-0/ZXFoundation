#pragma once

/// @brief branch prediction hints

#define __likely(x)     __builtin_expect(!!(x), 1)
#define __unlikely(x)   __builtin_expect(!!(x), 0)

#define likely(x)       __likely(x)
#define unlikely(x)       __unlikely(x)

/// @brief type traits

#define __is_array(a)		(!__same_type((a), &(a)[0]))

#define __is_byte_array(a)	(__is_array(a) && sizeof((a)[0]) == 1)

#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

#define statically_true(x) (__builtin_constant_p(x) && (x))

#define const_true(x) __builtin_choose_expr(__is_constexpr(x), x, false)

#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,				\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),									\
			 char:	(char)0,							\
			 __scalar_type_to_expr_cases(char),			\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),			\
			 __scalar_type_to_expr_cases(long),			\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))

#define __scalar_type_to_signed_cases(type)				\
		unsigned type:	(signed type)0,					\
		signed type:	(signed type)0

#define __signed_scalar_typeof(x) typeof(				\
		_Generic((x),									\
			 char:	(signed char)0,						\
			 __scalar_type_to_signed_cases(char),		\
			 __scalar_type_to_signed_cases(short),		\
			 __scalar_type_to_signed_cases(int),		\
			 __scalar_type_to_signed_cases(long),		\
			 __scalar_type_to_signed_cases(long long),	\
			 default: (x)))

/// @brief typeof(member) shorthand
///
///        Expands to the type of member @m inside struct/union @T without
///        requiring an actual instance of @T.  Equivalent to Linux's
///        typeof_member() and safe to use in unevaluated contexts.
#define typeof_member(T, m) typeof(((T *)0)->m)

// ---------------------------------------------------------------------------
// container_of / container_of_const
// ---------------------------------------------------------------------------

/// @brief Cast a pointer to a struct member back to the enclosing struct.
///
///        A compile-time `static_assert` verifies that the type of `*ptr`
///        is compatible with the type of `((type *)0)->member`, catching
///        mismatches that a bare cast would silently accept.
///
///        Use `container_of_const` in new code to preserve the const
///        qualifier of `ptr`.
///
/// @param ptr     Pointer to the embedded member.
/// @param type    Type of the enclosing struct.
/// @param member  Name of the member field within @type.
#define container_of(ptr, type, member) ({						\
	void *__mptr = (void *)(ptr);								\
	static_assert(__same_type(*(ptr), ((type *)0)->member) ||	\
		      __same_type(*(ptr), void),						\
		      "pointer type mismatch in container_of()");		\
	((type *)(__mptr - offsetof(type, member))); })

/// @brief const-preserving variant of container_of.
///
///        Uses `_Generic` to select either `const type *` or `type *`
///        depending on whether `ptr` points at a const-qualified object.
///        Always prefer this over container_of() in new code.
///
/// @param ptr     Pointer to the embedded member.
/// @param type    Type of the enclosing struct.
/// @param member  Name of the member field within @type.
#define container_of_const(ptr, type, member)						\
	_Generic(ptr,													\
		const typeof(*(ptr)) *: ((const type *)container_of(ptr, type, member)),\
		default:               ((type *)container_of(ptr, type, member))	\
	)

/// @brief compile time assert

# define __compiletime_error(msg)       __attribute__((__error__(msg)))

# define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {															\
		[[noreturn]] extern void prefix ## suffix(void)				\
			__compiletime_error(msg);								\
		if (!(condition))											\
			prefix ## suffix();										\
	} while (0)

#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)

#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __COUNTER__)

#define compiletime_assert_atomic_type(t)				\
	compiletime_assert(__native_word(t),				\
		"Need native word sized stores/loads for atomicity.")

#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#define compiletime_assert_rwonce_type(t)					\
	compiletime_assert(__native_word(t) || sizeof(t) == sizeof(long long),	\
		"sys: Unsupported access size for {READ,WRITE}_ONCE().")

#define __read_once(x)	(*(const volatile __unqual_scalar_typeof(x) *)&(x))
#define read_once(x)							\
	({											\
		compiletime_assert_rwonce_type(x);		\
		__read_once(x);							\
	})

#define __write_once(x, val)					\
	do {								        \
		*(volatile typeof(x) *)&(x) = (val);	\
	} while (0)

#define write_once(x, val)						\
	do {				     					\
		compiletime_assert_rwonce_type(x);		\
		__write_once(x, val);			    	\
	} while (0)