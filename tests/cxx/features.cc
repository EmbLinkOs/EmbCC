// CX8: the preprocessor's feature tests as libstdc++ uses them —
// __has_builtin (also from a macro's expansion, as _GLIBCXX_HAS_BUILTIN
// writes it), __has_include, __has_attribute, __has_cpp_attribute, each
// "defined" to #ifdef — and the __cpp_* feature-test macros of what EmbCC
// implements.
// expect-exit: 42
#include <stdio.h>

#ifndef __has_builtin
#error __has_builtin
#endif
#if !defined(__has_include) || !defined(__has_cpp_attribute)
#error defined
#endif
#if !__has_builtin(__builtin_expect) || __has_builtin(__no_such_builtin)
#error __has_builtin
#endif
#define HAS_BUILTIN(B) __has_builtin(B)
#if !HAS_BUILTIN(__builtin_unreachable)
#error __has_builtin after expansion
#endif
#if !__has_include(<stdio.h>) || __has_include(<no/such/header.h>)
#error __has_include
#endif
#if !__has_attribute(__noreturn__) || !__has_attribute(aligned)
#error __has_attribute
#endif
#if __has_cpp_attribute(nodiscard) != 201907L || \
    __has_cpp_attribute(maybe_unused) != 201603L
#error __has_cpp_attribute
#endif
#if __cpp_structured_bindings < 201606L || __cpp_if_constexpr < 201606L || \
    __cpp_fold_expressions < 201603L || __cpp_user_defined_literals < 200809L
#error feature-test macros
#endif
#if !defined(__cpp_exceptions) || !defined(__cpp_rtti) || !defined(__GXX_RTTI)
#error exceptions and RTTI
#endif

int main()
{
    printf("all ok\n");
    return 42;
}
