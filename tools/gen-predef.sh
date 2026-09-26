#!/bin/sh
# Regenerate a target's predefined-macro table from the reference compiler.
#
# ARCHITECTURE.md §5: the predefined-macro table is taken from
# `<triple>-gcc -dM -E </dev/null`, never hand-derived. This script is the
# only way those files may change; the generated tables are checked in so
# the build (and eventual self-hosting) does not depend on the cross
# toolchain.
#
# usage: gen-predef.sh [ARCH]                        regenerate (default: all)
#        gen-predef.sh --reference ARCH               print the filtered table
#                                                    (what tests/golden/predef.sh
#                                                    compares --dump-predef with)
#
#   ARCH is one of: x86_64 aarch64 thumb riscv32 riscv64
#
# The EMBEDDED targets -- `thumb` (ARMv7-M, Cortex-M) and the two RISC-V
# widths -- are taken from CLANG rather than gcc, because clang carries
# every target in one binary where each of arm-none-eabi-gcc and
# riscv64-elf-gcc is a separate toolchain download. It is the same kind of
# source -- a production compiler's own answer for the triple, read off
# rather than reasoned out -- and the generated file's header says which
# one it was. Set EMBCC_REF_GCC_THUMB / _RISCV32 / _RISCV64 to use a real
# cross gcc instead.
#
# riscv32 and riscv64 are asked for SEPARATELY, and with an explicit
# -march/-mabi, because the two differ in far more than __riscv_xlen: the
# type widths, the atomic lock-free set, and the C library's int-fast
# choices all move. `-march=rv32im` also pins what is being claimed -- the
# default rv32imafdc would define __riscv_flen, and EmbCC has no hardware
# float for the target (THE RULE).
#
# Excluded, each for THE RULE (claim only what is present):
#   __GNUC*__, __VERSION__   EmbCC is not gcc; defining these would switch
#                            real headers onto gcc-only extension paths.
#   __STDC*__                owned by the compiler proper, not the target;
#                            cpp defines them itself (see predef.h).
#   __BITINT_MAXWIDTH__      gcc 14+ advertises C23 _BitInt with it; EmbCC
#                            has no _BitInt, so a header testing it must not
#                            be told otherwise.
# -mcmodel=medany is asked for explicitly, and is not a detail either.
# Clang defaults to medlow and then defines __riscv_cmodel_medlow, but
# the backend emits MEDANY at both widths -- PC-relative auipc, because
# lui sign-extends bit 31 and the absolute pair cannot name a firmware
# image at 0x80000000 (D-016). The table said medlow for a compiler that
# emits medany, so code that switches on the macro to pick an addressing
# sequence picked the wrong one.
#
#   __riscv_v_intrinsic      clang defines it for plain rv32im/rv64im, with
#                            no __riscv_v beside it -- the version of a
#                            vector intrinsics API for a vector unit that
#                            is not in the -march. EmbCC has no vectors at
#                            any width, so it is the same overclaim.
set -eu

# __clang__/__llvm__ join the list for the same reason __GNUC__ is on it:
# EmbCC is not clang either, and a header that believes it is will take a
# path built on builtins this compiler does not have.
EXCLUDE='^#define (__GNUC|__VERSION__|__STDC|__BITINT_MAXWIDTH__|__clang|__llvm__|__riscv_v_intrinsic)'

refgcc() {
    gccvar=$(echo "EMBCC_REF_GCC_$1" | tr '[:lower:]' '[:upper:]')
    case "$1" in
        thumb|riscv32|riscv64) eval "echo \${$gccvar:-clang}" ;;
        *)                     eval "echo \${$gccvar:-$1-elf-gcc}" ;;
    esac
}

# The flags that pick the target when the reference compiler is not
# already specific to it. Empty for a cross gcc, which knows only one.
refflags() {
    case "$1" in
        thumb)   [ -n "${EMBCC_REF_GCC_THUMB:-}" ] || \
                     echo "-target thumbv7m-none-eabi -ffreestanding" ;;
        riscv32) [ -n "${EMBCC_REF_GCC_RISCV32:-}" ] || \
                     echo "-target riscv32-unknown-elf -march=rv32im -mabi=ilp32 -mcmodel=medany -ffreestanding" ;;
        riscv64) [ -n "${EMBCC_REF_GCC_RISCV64:-}" ] || \
                     echo "-target riscv64-unknown-elf -march=rv64im -mabi=lp64 -mcmodel=medany -ffreestanding" ;;
        *)       ;;
    esac
}

reference() {
    # shellcheck disable=SC2046
    "$(refgcc "$1")" $(refflags "$1") -dM -E - </dev/null \
        | LC_ALL=C sort | grep -v -E "$EXCLUDE"
}

# C++ (D-013): the g++ -std=gnu++20 set, less the same families, less every
# macro that CLAIMS a feature EmbCC's C++ does not implement yet — the __cpp_*
# feature tests, exceptions, RTTI, constexpr asm. libstdc++ switches on these,
# so claiming one early would compile code the front-end cannot. A pattern
# leaves this list when its feature lands (docs/language/cpp-levels.md). The implemented
# ones cpp defines itself (cpp_process), as exceptions depend on
# -fno-exceptions. Also excluded: the types EmbCC's C++ has not got —
# __int128 (and libstdc++'s __GLIBCXX_TYPE_INT_N_0 naming it), __float128,
# __float80 and the extended floating types (_Float16/32/64/128, __bf16).
EXCLUDE_CXX='^#define (__GNUG__|__cpp_|__EXCEPTIONS|__GXX_RTTI|__GXX_CONSTEXPR_ASM__|__GLIBCXX_|__SIZEOF_INT128__|__SIZEOF_FLOAT128__|__SIZEOF_FLOAT80__|__BFLT16_|__FLT16_|__FLT32_|__FLT32X_|__FLT64_|__FLT64X_|__FLT128_|__STDCPP_BFLOAT16|__STDCPP_FLOAT)'

refgxx() {
    case "$1" in
        thumb|riscv32|riscv64) refgcc "$1" | sed 's/clang$/clang++/' ;;
        *)                     refgcc "$1" | sed 's/gcc$/g++/' ;;
    esac
}

reference_cxx() {
    # shellcheck disable=SC2046
    "$(refgxx "$1")" $(refflags "$1") -std=gnu++20 -x c++ -dM -E - </dev/null \
        | LC_ALL=C sort | grep -v -E "$EXCLUDE" | grep -v -E "$EXCLUDE_CXX"
}

if [ "${1:-}" = --reference ]; then
    reference "${2:?usage: gen-predef.sh --reference ARCH}"
    exit 0
fi
if [ "${1:-}" = --reference-cxx ]; then
    reference_cxx "${2:?usage: gen-predef.sh --reference-cxx ARCH}"
    exit 0
fi

gen() {
    arch=$1
    GCC=$(refgcc "$arch")
    OUT="$(dirname "$0")/../src/arch/$arch/predef.c"

    command -v "$GCC" >/dev/null 2>&1 || {
        echo "gen-predef: reference compiler '$GCC' not found" >&2
        echo "gen-predef: set $gccvar or add it to PATH" >&2
        exit 1
    }

    {
        echo "/* Generated by tools/gen-predef.sh from \`$("$GCC" $(refflags "$arch") -dumpmachine) $(basename "$GCC") $("$GCC" -dumpversion) -dM -E\`."
        echo "   Do not edit by hand; rerun the script (ARCHITECTURE.md §5). */"
        echo
        echo "#include \"../predef.h\""
        echo
        echo "const struct predef_macro predef_macros_$arch[] = {"
        reference "$arch" \
            | sed -e 's/^#define \([^ ]*\) \(.*\)$/\1\x01\2/' \
            | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
            | sed -e 's/^\(.*\)\x01\(.*\)$/    { "\1", "\2" },/'
        echo "};"
        echo
        echo "const int predef_macro_count_$arch ="
        echo "    (int)(sizeof predef_macros_$arch / sizeof predef_macros_$arch[0]);"
    } > "$OUT"

    echo "gen-predef: wrote $OUT ($(grep -c '{ "' "$OUT") macros)"

    GXX=$(refgxx "$arch")
    OUT="$(dirname "$0")/../src/arch/$arch/predef_cxx.c"
    {
        echo "/* Generated by tools/gen-predef.sh from \`$("$GXX" $(refflags "$arch") -dumpmachine) $(basename "$GXX") $("$GXX" -dumpversion) -std=gnu++20 -dM -E\`,"
        echo "   less the macros claiming C++ features EmbCC does not implement yet"
        echo "   (EXCLUDE_CXX). Do not edit by hand; rerun the script. */"
        echo
        echo "#include \"../predef.h\""
        echo
        echo "const struct predef_macro predef_macros_cxx_$arch[] = {"
        reference_cxx "$arch" \
            | sed -e 's/^#define \([^ ]*\) \(.*\)$/\1\x01\2/' \
            | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
            | sed -e 's/^\(.*\)\x01\(.*\)$/    { "\1", "\2" },/'
        echo "};"
        echo
        echo "const int predef_macro_count_cxx_$arch ="
        echo "    (int)(sizeof predef_macros_cxx_$arch / sizeof predef_macros_cxx_$arch[0]);"
    } > "$OUT"
    echo "gen-predef: wrote $OUT ($(grep -c '{ "' "$OUT") macros)"
}

case "${1:-both}" in
    x86_64)  gen x86_64 ;;
    aarch64) gen aarch64 ;;
    thumb)   gen thumb ;;
    riscv32) gen riscv32 ;;
    riscv64) gen riscv64 ;;
    both|all) gen x86_64; gen aarch64; gen thumb; gen riscv32; gen riscv64 ;;
    *) echo "usage: $0 [x86_64|aarch64|thumb|riscv32|riscv64]" >&2; exit 1 ;;
esac
