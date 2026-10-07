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
#   ARCH is one of: x86_64 aarch64 thumb thumbv6m thumbv8m thumbv8mbase armv7a riscv32 riscv64 avr mips32
#                   mips32eb
#                   loongarch64
#                   xtensa
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
# The `a` in -march=rv32imac/rv64imac is the ATOMIC extension, and it is in
# the baseline because Hazard3 -- the RTOS requirements' fourth target, and
# the RP2350's RISC-V core -- is RV32IMAC. A kernel cannot be written without
# a compare-and-swap, and the backend emits lr/sc and the amo* family
# (src/arch/riscv/codegen.c). Claiming __riscv_atomic while refusing every
# atomic operation, which is what -march=rv32imc did, is the overclaim this
# file exists to avoid -- in the other direction.
#
# The `c` in -march=rv32imac/rv64imac is the compressed extension, which
# the backend now emits (src/arch/riscv/emit.c rv_compress). It is asked
# for here so __riscv_c and __riscv_compressed are defined, because code
# that tests them and gets the wrong answer picks the wrong instruction
# sizes -- a hand-written trampoline, a vector table, anything that
# counts bytes. NOT `a`: EmbCC lowers the atomic builtins without
# lr/sc, so claiming the A extension would be claiming instructions it
# never emits.
#
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
#
# __ARM_FEATURE_CMSE is NOT excluded: clang defines it as 1 for every ARMv8-M
# target (the TT instruction, which <arm_cmse.h> reads through cmse_TT), and
# EmbCC has both -- and with -mcmse src/arch/predef.c makes it 3, the Secure
# side, as clang does. It was filtered while embld minted no secure gateway
# veneer, so that a header seeing it would not write a cmse_nonsecure_entry
# that then failed somewhere else.
EXCLUDE='^#define (__GNUC|__VERSION__|__STDC|__BITINT_MAXWIDTH__|__clang|__llvm__|__riscv_v_intrinsic)'

refgcc() {
    gccvar=$(echo "EMBCC_REF_GCC_$1" | tr '[:lower:]' '[:upper:]')
    case "$1" in
        thumb|thumbv6m|thumbv8m|thumbv8mbase|armv7a|riscv32|riscv64|avr|mips32|mips32eb|loongarch64) eval "echo \${$gccvar:-clang}" ;;
        # Xtensa: Espressif's own GCC for the ESP32 (crosstool-NG release
        # esp-16.1.0_20260609), there being no Xtensa target in clang.
        xtensa)  eval "echo \${$gccvar:-xtensa-esp32-elf-gcc}" ;;
        *)                     eval "echo \${$gccvar:-$1-elf-gcc}" ;;
    esac
}

# The flags that pick the target when the reference compiler is not
# already specific to it. Empty for a cross gcc, which knows only one.
refflags() {
    case "$1" in
        thumb)   [ -n "${EMBCC_REF_GCC_THUMB:-}" ] || \
                     echo "-target thumbv7m-none-eabi -ffreestanding" ;;
        # ARMv8-M Mainline (Cortex-M33). A SECOND table rather than the v7-M
        # one with __ARM_ARCH patched: the two differ in far more than the
        # architecture number -- the feature macros (__ARM_FEATURE_*), the
        # CMSE ones and the DSP flags all move -- and a generated file has no
        # business being hand-edited into a parameterised one
        # (ARCHITECTURE.md §5). Same reason the two RISC-V widths have two.
        #
        # -mfloat-abi=soft, which the v7-M line gets for free because
        # thumbv7m has no FPU by default and thumbv8m.main does. Without it
        # this table said `__ARM_FP 0xe` -- a hardware FPU with single AND
        # double precision -- and no `__SOFTFP__`, on a backend that does
        # every float operation as a call. The first thing that read it was
        # lib/rt/softfp.c's own guard, which compiled to NOTHING for this
        # target: `a + b` on two floats called __addsf3 and nothing anywhere
        # defined it, so no float program linked for a Cortex-M33 at all.
        # A predefined macro is a promise to the program; this one promised
        # hardware the generated code never uses. When the hard-float ABI
        # lands, it changes here and in the backend together.
        # ARMv6-M (Cortex-M0/M0+/M1): its own table for the same reason.
        # No FPU exists for it, so soft float is the only answer, and the
        # atomic lock-free values are 1: the backend calls __atomic_* for a
        # read-modify-write, as clang does, having no exclusives to inline.
        thumbv6m) [ -n "${EMBCC_REF_GCC_THUMBV6M:-}" ] || \
                     echo "-target thumbv6m-none-eabi -ffreestanding" ;;
        thumbv8m) [ -n "${EMBCC_REF_GCC_THUMBV8M:-}" ] || \
                     echo "-target thumbv8m.main-none-eabi -mfloat-abi=soft -ffreestanding" ;;
        # ARMv8-M Baseline (Cortex-M23): its own table, as v6m and v8m have.
        # No FPU exists for it either. Its lock-free values are 2: the
        # exclusives are there, and the backend inlines a one-, two- or
        # four-byte atomic as an ldrex/strex loop (v6m.c).
        thumbv8mbase) [ -n "${EMBCC_REF_GCC_THUMBV8MBASE:-}" ] || \
                     echo "-target thumbv8m.base-none-eabi -mcpu=cortex-m23 -ffreestanding" ;;
        # ARMv7-A in ARM state. -mfloat-abi=soft for the reason thumbv8m
        # takes it: clang's default for this triple is VFPv3 with NEON
        # (__ARM_FP, __ARM_NEON), and the backend does every float
        # operation as a call.
        armv7a)  [ -n "${EMBCC_REF_GCC_ARMV7A:-}" ] || \
                     echo "-target armv7a-none-eabi -mfloat-abi=soft -ffreestanding" ;;
        riscv32) [ -n "${EMBCC_REF_GCC_RISCV32:-}" ] || \
                     echo "-target riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding" ;;
        riscv64) [ -n "${EMBCC_REF_GCC_RISCV64:-}" ] || \
                     echo "-target riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding" ;;
        # AVR names the PART, not just the architecture: __AVR_ATmega328P__
        # and the __AVR_HAVE_* feature macros all come from -mmcu=, and a
        # header that tests them is how AVR code is normally written. The
        # Nano profile in the requirements document is an ATmega328P.
        avr)     [ -n "${EMBCC_REF_GCC_AVR:-}" ] || \
                     echo "-target avr -mmcu=atmega328p -ffreestanding" ;;
        # MIPS32r2, little-endian, o32, soft float: a PIC32's core.
        # -mno-abicalls because the backend's code is not abicalls: it
        # takes addresses with absolute lui/addiu pairs, calls with jal,
        # keeps no $gp and writes no EF_MIPS_CPIC -- so __mips_abicalls,
        # which clang defines by default for this triple, would claim a
        # convention the objects do not follow. It is the only macro the
        # flag changes.
        mips32)  [ -n "${EMBCC_REF_GCC_MIPS32:-}" ] || \
                     echo "-target mipsel-unknown-elf -mcpu=mips32r2 -msoft-float -mno-abicalls -ffreestanding" ;;
        # The same core BIG-endian (mips-none-elf): its own table, as the
        # byte-order macros (__BYTE_ORDER__, __BIG_ENDIAN__, MIPSEB and the
        # _MIPSEB family) are the generated answer, not a patch on mipsel's.
        mips32eb) [ -n "${EMBCC_REF_GCC_MIPS32EB:-}" ] || \
                     echo "-target mips-unknown-elf -mcpu=mips32r2 -msoft-float -mno-abicalls -ffreestanding" ;;
        # LoongArch64, LP64S. -msoft-float is -mabi=lp64s AND -mfpu=none:
        # with the ABI alone clang still claims __loongarch_frlen 64 and the
        # LSX vector unit (__loongarch_sx), hardware the soft-float code
        # never touches.
        loongarch64) [ -n "${EMBCC_REF_GCC_LOONGARCH64:-}" ] || \
                     echo "-target loongarch64-unknown-elf -msoft-float -ffreestanding" ;;
        *)       ;;
    esac
}

# Per-ARCH exclusions, for a macro that is legitimate on one target and an
# overclaim on another.
#
# RISC-V: __GCC_HAVE_SYNC_COMPARE_AND_SWAP_1 and _2 claim one- and two-byte
# atomics. The A extension has no such instruction -- it provides .w and, at
# RV64, .d and nothing narrower -- so the backend refuses them rather than
# doing a read-modify-write of the containing word, which would not be atomic
# against a neighbouring byte. gcc answers these by calling libatomic; EmbCC
# has no such library, so claiming them would make a program compile and then
# fail to link. _4 (and _8 at RV64) stay: those are real.
exclude_arch() {
    case "$1" in
        riscv32) echo '^#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_(1|2|8)' ;;
        riscv64) echo '^#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_(1|2)' ;;
        # MIPS32's ll/sc are word-sized, and the backend refuses a one- or
        # two-byte atomic exactly as RISC-V's does (no libatomic here).
        mips32|mips32eb) echo '^#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_(1|2)' ;;
        # (LoongArch64 claims all four: its backend makes a one- or two-byte
        # atomic an ll.w/sc.w loop on the word, as clang does.)
        # ARMv7-A has ldrexd/strexd, so clang claims an eight-byte
        # compare-and-swap; the backend refuses an eight-byte atomic by name
        # (as on ARMv7-M, which has no ldrexd), so this does not claim it.
        armv7a)  echo '^#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8' ;;
        # ARMv8-M Baseline: clang defines the ARMv8 feature macros it has
        # for every v8 architecture, and five of them claim what Baseline
        # does not have -- CLZ, the saturating instructions (SAT) and the
        # Q flag they set (QBIT) are Thumb-2 DSP-class instructions this
        # core lacks, and NUMERIC_MAXMIN and DIRECTED_ROUNDING are VFP
        # instructions on a core with no FPU. GCC defines none of the five
        # for armv8-m.base. A program that tests __ARM_FEATURE_CLZ and
        # writes `clz` in asm would get an UNDEFINED instruction.
        thumbv8mbase) echo '^#define __ARM_FEATURE_(CLZ|QBIT|SAT|NUMERIC_MAXMIN|DIRECTED_ROUNDING) ' ;;
        # Xtensa's s32c1i is word-sized too. The table is the ESP32's
        # (xtensa-esp32-elf-gcc, which has no -msoft-float: the float ABI
        # is the same either way, every float in the address registers).
        xtensa)  echo '^#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_(1|2)' ;;
        *)       echo 'ZZZ_NO_SUCH_MACRO_ZZZ' ;;
    esac
}

# Macros clang computes in its preprocessor instead of predefining, so
# `clang -dM` does not list them, but which <float.h> expands to and a
# program may use in an expression. A gcc reference lists them with these
# same values, and `sort -u` folds the two.
computed() {
    echo '#define __FLT_EVAL_METHOD_TS_18661_3__ 0'
    echo '#define __FLT_EVAL_METHOD__ 0'
}

reference() {
    # shellcheck disable=SC2046
    { "$(refgcc "$1")" $(refflags "$1") -dM -E - </dev/null; computed; } \
        | LC_ALL=C sort -u | grep -v -E "$EXCLUDE" \
        | grep -v -E "$(exclude_arch "$1")"
}

# C++ (D-013): the g++ -std=gnu++20 set, less the same families, less every
# macro that CLAIMS a feature EmbCC's C++ does not implement yet — the __cpp_*
# feature tests, exceptions, RTTI, constexpr asm. libstdc++ switches on these,
# so claiming one early would compile code the front-end cannot. A pattern
# leaves this list when its feature lands (docs/manual/cxx.md). The implemented
# ones cpp defines itself (cpp_process), as exceptions depend on
# -fno-exceptions. Also excluded: the types EmbCC's C++ has not got —
# __int128 (and libstdc++'s __GLIBCXX_TYPE_INT_N_0 naming it), __float128,
# __float80 and the extended floating types (_Float16/32/64/128, __bf16).
EXCLUDE_CXX='^#define (__GNUG__|__cpp_|__EXCEPTIONS|__GXX_RTTI|__GXX_CONSTEXPR_ASM__|__GLIBCXX_|__SIZEOF_INT128__|__SIZEOF_FLOAT128__|__SIZEOF_FLOAT80__|__BFLT16_|__FLT16_|__FLT32_|__FLT32X_|__FLT64_|__FLT64X_|__FLT128_|__STDCPP_BFLOAT16|__STDCPP_FLOAT)'

refgxx() {
    case "$1" in
        thumb|thumbv6m|thumbv8m|thumbv8mbase|armv7a|riscv32|riscv64|mips32|mips32eb|loongarch64) refgcc "$1" | sed 's/clang$/clang++/' ;;
        *)                     refgcc "$1" | sed 's/gcc$/g++/' ;;
    esac
}

# The per-arch exclusions the C++ table takes too. Only Baseline's: they
# are claims about INSTRUCTIONS, which a C++ program reads the same way.
# (The atomics ones above have never been applied to the C++ tables, and
# changing those tables is a separate decision.)
exclude_arch_cxx() {
    case "$1" in
        thumbv8mbase) exclude_arch "$1" ;;
        *)            echo 'ZZZ_NO_SUCH_MACRO_ZZZ' ;;
    esac
}

reference_cxx() {
    # shellcheck disable=SC2046
    { "$(refgxx "$1")" $(refflags "$1") -std=gnu++20 -x c++ -dM -E - \
        </dev/null; computed; } \
        | LC_ALL=C sort -u | grep -v -E "$EXCLUDE" | grep -v -E "$EXCLUDE_CXX" \
        | grep -v -E "$(exclude_arch_cxx "$1")"
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
    thumbv6m) gen thumbv6m ;;
    thumbv8m) gen thumbv8m ;;
    thumbv8mbase) gen thumbv8mbase ;;
    armv7a)  gen armv7a ;;
    riscv32) gen riscv32 ;;
    riscv64) gen riscv64 ;;
    avr)     gen avr ;;
    mips32)  gen mips32 ;;
    mips32eb) gen mips32eb ;;
    loongarch64) gen loongarch64 ;;
    xtensa)  gen xtensa ;;
    both|all) gen x86_64; gen aarch64; gen thumb; gen thumbv6m; gen thumbv8m; gen thumbv8mbase; gen armv7a; gen riscv32
              gen riscv64; gen avr; gen mips32; gen mips32eb; gen loongarch64; gen xtensa ;;
    *) echo "usage: $0 [x86_64|aarch64|thumb|thumbv6m|thumbv8m|thumbv8mbase|armv7a|riscv32|riscv64|avr|mips32|mips32eb|loongarch64|xtensa]" >&2
       exit 1 ;;
esac
