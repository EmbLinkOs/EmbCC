#!/bin/sh
# The __has_* feature-test operators, in C.
#
# All of this machinery existed and was gated on one line in
# src/cpp/cpp.c -- `if (!predef_is_cxx()) return 0;`. It had been built
# for libstdc++ and C never got it, so in C:
#
#     #if defined(__has_include) && __has_include(<foo.h>)
#
# did not fall back gracefully. It was a SYNTAX ERROR: the undefined
# identifier became 0 and the expression parser then met `0 (<foo.h>)`
# and said "trailing junk in #if expression". A header written
# defensively failed to compile, which is strictly worse than the
# feature being absent -- the defensive spelling exists precisely so
# that compilers without it still work.
#
# The other half is that an answer must be TRUE. __has_builtin in C
# used to route to the C++ front end's table, which is NULL in C, so it
# would have answered 0 for a compiler that implements __builtin_clz
# and forty others. The last section here is the drift guard: it reads
# the builtin names out of sema's own dispatch and requires
# __has_builtin to agree about every one.
set -u
echo "TEST-MARKER has-feature"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/has-feature
rm -rf "$out"; mkdir -p "$out"
fail=0

# ask EXPR LANG WANT -- preprocess a guarded #if and read which arm won
ask() {
    ext=c; [ "${2:-c}" = c++ ] && ext=cc
    printf '#if %s\nint R_yes;\n#else\nint R_no;\n#endif\n' "$1" > "$out/q.$ext"
    got=$("$EMBCC" -E "$out/q.$ext" 2>&1)
    case "$got" in
        *R_yes*) got=yes ;;
        *R_no*)  got=no ;;
        *)       got="ERROR: $(printf '%s' "$got" | grep -oE 'error.*' |
                               head -1 | cut -c1-46)" ;;
    esac
    if [ "$got" = "$3" ]; then
        printf '  %-52s %s\n' "$1" "$got"
    else
        printf 'FAIL %-49s got %s, wanted %s\n' "$1" "$got" "$3"
        fail=1
    fi
}

echo "-- the guarded spelling every portable header uses --"
ask 'defined(__has_include) && __has_include(<stddef.h>)' c yes
ask 'defined(__has_include) && __has_include(<no_such_header_xyz.h>)' c no
ask 'defined(__has_builtin) && __has_builtin(__builtin_clz)' c yes

echo "-- and each operator on its own --"
ask '__has_include(<stddef.h>)'          c yes
ask '__has_include("no_such_xyz.h")'     c no
ask '__has_builtin(__builtin_clz)'       c yes
ask '__has_builtin(__builtin_popcountll)' c yes
ask '__has_builtin(__atomic_load_n)'     c yes
ask '__has_builtin(__builtin_no_such_thing)' c no
ask '__has_attribute(packed)'            c yes
ask '__has_attribute(no_such_attr_xyz)'  c no
ask '__has_feature(c_static_assert)'     c yes
ask '__has_feature(no_such_feature_xyz)' c no

echo "-- the C / C++ split, which is the one real difference --"
# __has_c_attribute is C's and __has_cpp_attribute is C++'s, as in both
# reference compilers. Each must be UNDEFINED in the other language, or
# the guarded spelling silently takes the wrong branch.
for pair in "__has_c_attribute:c:yes" "__has_cpp_attribute:c:no" \
            "__has_c_attribute:c++:no" "__has_cpp_attribute:c++:yes"; do
    op=${pair%%:*}; rest=${pair#*:}; lang=${rest%%:*}; want=${rest#*:}
    ext=c; [ "$lang" = c++ ] && ext=cc
    printf '#ifdef %s\nint R_yes;\n#else\nint R_no;\n#endif\n' "$op" \
        > "$out/d.$ext"
    got=no; "$EMBCC" -E "$out/d.$ext" 2>/dev/null | grep -q R_yes && got=yes
    if [ "$got" = "$want" ]; then
        printf '  %-28s defined in %-4s %s\n' "$op" "$lang" "$got"
    else
        printf 'FAIL %-24s in %s: got %s, wanted %s\n' "$op" "$lang" "$got" "$want"
        fail=1
    fi
done

# C++ must not have regressed: this all worked there before.
ask 'defined(__has_include) && __has_include(<stddef.h>)' c++ yes
ask '__has_builtin(__builtin_expect)' c++ yes

echo "-- drift: sema's dispatch vs what __has_builtin claims --"
# sema lowers most builtins through a chain of strcmp(bn, "name"), and
# sema_has_builtin restates that half as a list. Read the names out of
# the source and require __has_builtin to agree about every one, so the
# list cannot rot while the dispatch grows.
grep -ohE 'strcmp\(bn, "[a-z0-9_]+"\)' "$EMBCC_ROOT/src/sema/sema.c" \
     "$EMBCC_ROOT/src/ir/irgen.c" 2>/dev/null \
  | grep -oE '"[a-z0-9_]+"' | tr -d '"' | sort -u > "$out/dispatch.txt"
n=$(wc -l < "$out/dispatch.txt" | tr -d ' ')
[ "$n" -gt 10 ] || { echo "FAIL: found only $n builtins in the dispatch --"
                     echo "      the scan pattern has drifted from the source"
                     exit 1; }
: > "$out/drift.c"
while read -r bn; do
    printf '#if !__has_builtin(__builtin_%s)\n#error "__builtin_%s is lowered by sema but __has_builtin says no"\n#endif\n' \
        "$bn" "$bn" >> "$out/drift.c"
done < "$out/dispatch.txt"
if "$EMBCC" -E "$out/drift.c" > /dev/null 2> "$out/drift.err"; then
    echo "  $n builtins in sema's dispatch, __has_builtin agrees on all"
else
    echo "FAIL: __has_builtin disagrees with the dispatch:"
    grep -oE '"__builtin_.*"' "$out/drift.err" | head -6 | sed 's/^/     | /'
    fail=1
fi

# The other direction: everything the list claims must really compile.
# An unknown name is rejected as "is not declared"; a known one with the
# wrong argument count fails differently, and that is enough to tell
# them apart without knowing each builtin's arity.
# This direction found a real one: the overflow builtins were listed
# because a grep of the tree found them -- in the C++ front end. C has
# never implemented them, so `__has_builtin(__builtin_add_overflow)`
# would have said yes and the call would then not compile. A claim the
# compiler cannot honour is worse than a missing feature, which is the
# same principle as the __has_include fallback above.
miss=0
printf 'void f(void){ __builtin_clz(1); }\n' > "$out/sane.c"
"$EMBCC" -fsyntax-only "$out/sane.c" >/dev/null 2>&1 || {
    echo "FAIL: the discriminator itself is broken"; fail=1; }
for bn in alloca alloca_with_align assume_aligned bswap16 bswap32 bswap64 \
          constant_p expect expect_with_probability frame_address \
          return_address huge_val inf inff infl nan nanf nanl \
          memcpy memmove memset offsetof prefetch sqrt sqrtf sqrtl \
          trap unreachable va_arg va_copy va_end va_start; do
    printf 'void f(void){ __builtin_%s(); }\n' "$bn" > "$out/e.c"
    if "$EMBCC" -fsyntax-only "$out/e.c" 2>&1 | grep -q "is not declared"; then
        echo "FAIL: __has_builtin claims __builtin_$bn, the compiler does not know it"
        miss=1; fail=1
    fi
done
[ "$miss" -eq 0 ] && echo "  every listed builtin is one the compiler really knows"
# The overflow builtins were listed here first from a grep that found
# the C++ implementation, and this direction caught it: C did not have
# them and __has_builtin promised otherwise. C lowers them now
# (src/ir/irgen.c gen_overflow, tests/golden/overflow-builtins.sh), so
# the claim and the compiler have to agree in the other direction --
# both must say yes.
for bn in add_overflow sub_overflow mul_overflow; do
    printf '#if !__has_builtin(__builtin_%s)\n#error "C implements it but __has_builtin says no"\n#endif\n' \
        "$bn" > "$out/n.c"
    "$EMBCC" -E "$out/n.c" >/dev/null 2>&1 || {
        echo "FAIL: __has_builtin denies __builtin_$bn, which C now has"
        fail=1; }
    printf 'int f(int a,int b,int*r){return __builtin_%s(a,b,r);}\n' "$bn" \
        > "$out/n2.c"
    "$EMBCC" -fsyntax-only "$out/n2.c" >/dev/null 2>&1 || {
        echo "FAIL: __has_builtin claims __builtin_$bn but it does not compile"
        fail=1; }
done
echo "  the overflow builtins are claimed and really compile"

[ "$fail" -eq 0 ] || exit 1
