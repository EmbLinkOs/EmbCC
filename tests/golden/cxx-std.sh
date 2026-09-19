#!/bin/sh
# C++'s -std=: __cplusplus as g++ gives each standard, and each feature
# macro EmbCC claims defined in exactly the standards g++ defines it in
# (its value may be lower: what EmbCC does) — libstdc++'s sources, built
# per standard, choose their code (and so their symbols) by them.
set -u
echo "TEST-MARKER cxx-std"
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then REF=$AARCH64_REF_GXX; else REF=$X86_REF_GXX; fi
GXX=$REF/bin/$TARGET-g++
[ -x "$GXX" ] || { echo "skipped: no reference g++ at $REF"; exit 0; }
out=$EMBCC_ROOT/tests/golden/out/cxx-std-$ARCH
mkdir -p "$out"
macros="__cplusplus __cpp_rtti __cpp_alias_templates __cpp_aligned_new
__cpp_attributes __cpp_binary_literals __cpp_capture_star_this
__cpp_char8_t __cpp_concepts __cpp_constexpr __cpp_decltype
__cpp_decltype_auto __cpp_deduction_guides __cpp_delegating_constructors
__cpp_fold_expressions __cpp_generic_lambdas __cpp_if_constexpr
__cpp_impl_coroutine __cpp_impl_three_way_comparison __cpp_init_captures
__cpp_initializer_lists __cpp_inline_variables __cpp_lambdas __cpp_nsdmi
__cpp_range_based_for __cpp_ref_qualifiers __cpp_return_type_deduction
__cpp_rvalue_references __cpp_static_assert __cpp_structured_bindings
__cpp_unicode_literals __cpp_user_defined_literals __cpp_using_enum
__cpp_variable_templates __cpp_variadic_templates
__STDCPP_DEFAULT_NEW_ALIGNMENT__ __GXX_EXPERIMENTAL_CXX0X__"
src=$out/m.cc
: > "$src"
for m in $macros; do
    printf '%s %s\n' "$m" "$m" >> "$src"
done
fails=0
for std in gnu++98 gnu++11 gnu++14 gnu++17 gnu++20 gnu++23 c++17; do
    "$EMBCC" --target="$TARGET" -std=$std -E "$src" | grep -v '^#' |
        grep . > "$out/embcc-$std.txt"
    "$GXX" -std=$std -E -P "$src" | grep . > "$out/gxx-$std.txt"
    # a line "NAME VALUE": defined when VALUE is not the name itself
    paste -d' ' "$out/embcc-$std.txt" "$out/gxx-$std.txt" |
    while read -r name ev gname gv; do
        edef=$([ "$ev" = "$name" ] && echo 0 || echo 1)
        gdef=$([ "$gv" = "$gname" ] && echo 0 || echo 1)
        if [ "$name" = __cplusplus ] && [ "$ev" != "$gv" ]; then
            echo "-std=$std: __cplusplus $ev, g++ $gv"
        elif [ "$edef" != "$gdef" ]; then
            echo "-std=$std: $name $([ $edef = 1 ] && echo "defined ($ev)" || echo undefined), g++ $([ $gdef = 1 ] && echo "defined ($gv)" || echo undefined)"
        fi
    done > "$out/diff-$std.txt"
    if [ -s "$out/diff-$std.txt" ]; then
        cat "$out/diff-$std.txt"
        fails=1
    else
        echo "-std=$std: as g++"
    fi
done
[ "$fails" -eq 0 ]
