#!/bin/sh
# The predefined-macro table (ARCHITECTURE.md §5). Golden test: when the
# reference compiler is available, EmbCC's table must match it exactly
# (minus the excluded __GNUC*/__STDC* families — see tools/gen-predef.sh).
# Without it, fall back to the macros whose absence made newlib's headers
# hard-#error under TCC.
set -u
echo "TEST-MARKER predef"
. "$(dirname "$0")/../lib.sh"

# Each target's table must equal its reference compiler's, filtered by the
# ONE exclusion list in tools/gen-predef.sh (asked for directly, not restated
# here, so the test and the generator cannot drift apart).
checked=0
for arch in x86_64 aarch64 thumb thumbv6m thumbv8m thumbv8mbase armv7a riscv32 riscv64 avr mips32 mips32eb loongarch64 tricore xtensa ppc32 sparc32 mips64 mips64eb; do
    # The triple is not always "<arch>-elf": ARMv7-M is spelled the way
    # every other toolchain spells it, and gen-predef.sh keys on the short
    # name, so the two are named apart here rather than assumed equal.
    case $arch in
        thumb)          triple=thumbv7m-none-eabi ;;
        thumbv6m)       triple=thumbv6m-none-eabi ;;
        thumbv8m)       triple=thumbv8m.main-none-eabi ;;
        thumbv8mbase)   triple=thumbv8m.base-none-eabi ;;
        armv7a)         triple=armv7a-none-eabi ;;
        riscv32|riscv64) triple=$arch-unknown-elf ;;
        avr)            triple=avr ;;
        mips32)         triple=mipsel-none-elf ;;
        mips32eb)       triple=mips-none-elf ;;
        mips64)         triple=mips64el-none-elf ;;
        mips64eb)       triple=mips64-none-elf ;;
        loongarch64)    triple=loongarch64-unknown-elf ;;
        tricore)        triple=tricore-none-elf ;;
        xtensa)         triple=xtensa-none-elf ;;
        ppc32)          triple=powerpc-none-eabi ;;
        sparc32)        triple=sparc-none-elf ;;
        *)              triple=$arch-elf ;;
    esac
    out=$("$EMBCC" --target=$triple --dump-predef) || {
        echo "--target=$triple --dump-predef exited nonzero"; exit 1; }
    gcc=$(sh tools/gen-predef.sh --reference "$arch" 2>/dev/null >/dev/null && echo yes)
    if [ "$gcc" = yes ]; then
        ref=$(sh tools/gen-predef.sh --reference "$arch")
        if [ "$out" != "$ref" ]; then
            echo "$arch table disagrees with its reference gcc:"
            printf '%s\n' "$out" > "${TMPDIR:-/tmp}/predef.embcc.$$"
            printf '%s\n' "$ref" | diff -u - "${TMPDIR:-/tmp}/predef.embcc.$$"
            rm -f "${TMPDIR:-/tmp}/predef.embcc.$$"
            exit 1
        fi
        echo "$arch matches its reference gcc -dM -E ($(printf '%s\n' "$out" | wc -l | tr -d ' ') macros)"
        checked=$((checked + 1))
    else
        # No reference compiler: check the macros whose absence made newlib's
        # headers hard-#error under TCC (patch 0002), plus the arch's own.
        # __LP64__ is NOT in the shared list: ARMv7-M is ILP32 and must not
        # define it, so asking every target for it would demand the wrong
        # answer from one of them.
        case $arch in
            x86_64)  own="__x86_64__ __LP64__" ;;
            aarch64) own="__aarch64__ __LP64__" ;;
            thumb|thumbv6m|thumbv8m|thumbv8mbase) own="__arm__ __thumb__ __ARM_EABI__ __CHAR_UNSIGNED__" ;;
            armv7a) own="__arm__ __ARM_ARCH_7A__ __ARM_EABI__ __CHAR_UNSIGNED__" ;;
            riscv32) own="__riscv __riscv_xlen __riscv_float_abi_soft __CHAR_UNSIGNED__" ;;
            riscv64) own="__riscv __riscv_xlen __riscv_float_abi_soft __LP64__" ;;
            # AVR is the one target where an `int` is two bytes and a
            #  POINTER is two bytes, so __SIZEOF_INT__ is in its own list:
            #  asking every target for "int is 2" would demand the wrong
            #  answer from the other five. __AVR_ATmega328P__ is here
            #  because AVR code selects on the PART, not the family.
            avr)     own="__AVR__ __AVR_ARCH__ __AVR_ATmega328P__ __SIZEOF_INT__" ;;
            mips32)  own="__mips__ _MIPSEL __mips_soft_float __mips_o32 _MIPS_SZPTR" ;;
            mips32eb) own="__mips__ _MIPSEB __BIG_ENDIAN__ __mips_soft_float __mips_o32" ;;
            mips64)  own="__mips__ __mips64 _MIPSEL __mips_soft_float __mips_n64 __LP64__" ;;
            mips64eb) own="__mips__ __mips64 _MIPSEB __mips_soft_float __mips_n64 __LP64__" ;;
            loongarch64) own="__loongarch__ __loongarch64 __loongarch_soft_float __loongarch_lp64 __LP64__" ;;
            # (there is no TriCore compiler to generate the table from:
            #  src/arch/tricore/predef.c says how it was made)
            tricore) own="__tricore__ __TRICORE__ __TRICORE_CORE__ __ILP32__" ;;
            xtensa)  own="__xtensa__ __XTENSA__ __XTENSA_EL__ __XTENSA_WINDOWED_ABI__ __CHAR_UNSIGNED__ __SIZEOF_WCHAR_T__" ;;
            ppc32)   own="__PPC__ _ARCH_PPC __BIG_ENDIAN__ _SOFT_FLOAT __CHAR_UNSIGNED__" ;;
            sparc32) own="__sparc__ __sparcv8 __BIG_ENDIAN__ SOFT_FLOAT __SIZEOF_LONG_DOUBLE__" ;;
        esac
        for m in __INT64_TYPE__ __INTPTR_TYPE__ __SIZE_TYPE__ __PTRDIFF_TYPE__ \
                 __CHAR_BIT__ __SIZEOF_POINTER__ __SIZEOF_LONG__ \
                 __ELF__ $own; do
            echo "$out" | grep -q "^#define $m " || {
                echo "$arch: missing $m (the TCC-patch-0002 class of break)"
                exit 1
            }
        done
        n=$(printf '%s\n' "$out" | wc -l)
        [ "$n" -ge 300 ] || { echo "$arch: only $n macros — table looks truncated"; exit 1; }
        echo "$arch: no reference gcc; the known-fatal macros are present"
    fi
done

# RISC-V's -march= and -mabi=: every hardware-float combination, and the C
# extension left out, against clang given the same two flags (the macros
# are a promise about the code: __riscv_flen says what the FPU is, and
# __riscv_float_abi_* where floating point is passed). Without -mabi= the
# ABI follows -march= as clang's does. Sorted: the overrides are appended.
if command -v clang >/dev/null 2>&1; then
    tmpa=${TMPDIR:-/tmp}/predef.rv.$$
    tmpb=${TMPDIR:-/tmp}/predef.rvref.$$
    nrv=0
    for combo in "32 rv32imafc ilp32f" "32 rv32imafc ilp32" "32 rv32imafdc ilp32d" \
                 "32 rv32imafdc ilp32f" "32 rv32gc ilp32d" "32 rv32imaf ilp32f" \
                 "32 rv32ima ilp32" "32 rv32imafdc_zicsr_zifencei ilp32d" \
                 "32 rv32imafc -" "32 rv32gc -" \
                 "64 rv64gc lp64d" "64 rv64imafc lp64f" "64 rv64imafdc lp64" \
                 "64 rv64imafdc lp64f" "64 rv64ima lp64" "64 rv64gc -"; do
        set -- $combo
        abiflag="-mabi=$3"; refabi=$3
        if [ "$3" = - ]; then
            abiflag=""
            case $2 in *d*|rv*g*) refabi=$( [ $1 = 32 ] && echo ilp32d || echo lp64d ) ;;
                       *f*) refabi=$( [ $1 = 32 ] && echo ilp32f || echo lp64f ) ;;
            esac
        fi
        # shellcheck disable=SC2086
        "$EMBCC" --target=riscv$1-unknown-elf -march=$2 $abiflag \
            --dump-predef | LC_ALL=C sort > "$tmpa" || {
            echo "riscv$1 -march=$2 $abiflag: --dump-predef failed"; exit 1; }
        EMBCC_PREDEF_RV_MARCH=$2 EMBCC_PREDEF_RV_MABI=$refabi \
            sh tools/gen-predef.sh --reference riscv$1 | LC_ALL=C sort > "$tmpb"
        if ! cmp -s "$tmpa" "$tmpb"; then
            echo "riscv$1 -march=$2 $abiflag disagrees with clang:"
            diff "$tmpb" "$tmpa" | head -20
            rm -f "$tmpa" "$tmpb"; exit 1
        fi
        nrv=$((nrv + 1))
    done
    rm -f "$tmpa" "$tmpb"
    echo "RISC-V -march/-mabi: $nrv combinations match clang -dM -E"
fi

# The Cortex-M parts and architectures, against clang given the same flags:
# every __ARM_ARCH* and __ARM_FEATURE_* macro. The generated tables are the
# bare thumbv7m and thumbv8m.main ones; -mcpu= and -march= (and the
# thumbv7em name) move the architecture and add the DSP extension's macros
# (__ARM_ARCH_7EM__, __ARM_FEATURE_DSP, __ARM_FEATURE_SIMD32), which
# CMSIS's cmsis_gcc.h selects its __SADD16 ... __SMLALD on. A DSP macro on
# a v7-M or v6-M part, or a missing one on a v7E-M part, is a difference
# here. __ARM_FEATURE_FMA is left out of both sides: clang claims it with
# no FPU, and EmbCC does not (src/arch/predef.c, thumb_fpu_add).
if command -v clang >/dev/null 2>&1; then
    tmpa=${TMPDIR:-/tmp}/predef.cm.$$
    tmpb=${TMPDIR:-/tmp}/predef.cmref.$$
    ncm=0
    for combo in "thumbv7em-none-eabi" "thumbv7m-none-eabi" \
                 "thumbv6m-none-eabi" "thumbv8m.main-none-eabi" \
                 "thumbv8m.base-none-eabi -mcpu=cortex-m23" \
                 "thumbv7em-none-eabihf" "thumbv7em-none-eabi -mcpu=cortex-m4" \
                 "thumbv7em-none-eabi -mcpu=cortex-m7" \
                 "thumbv7em-none-eabi -mcpu=cortex-m3" \
                 "thumbv7em-none-eabi -mcpu=cortex-m0" \
                 "thumbv7em-none-eabi -mcpu=cortex-m33" \
                 "thumbv7m-none-eabi -mcpu=cortex-m4" \
                 "thumbv6m-none-eabi -mcpu=cortex-m7" \
                 "thumbv8m.main-none-eabi -mcpu=cortex-m33" \
                 "thumbv8m.main-none-eabi -mcpu=cortex-m4" \
                 "thumbv8m.main-none-eabihf -mcpu=cortex-m33" \
                 "thumbv7m-none-eabi -march=armv7e-m" \
                 "thumbv7em-none-eabi -march=armv7-m" \
                 "thumbv7m-none-eabi -march=armv8-m.main+dsp" \
                 "thumbv8m.main-none-eabi -march=armv8-m.main+nodsp" \
                 "thumbv7em-none-eabi -mcpu=cortex-m4 -x c++"; do
        set -- $combo
        t=$1; shift
        # shellcheck disable=SC2086
        "$EMBCC" --target=$t "$@" -E -dM - < /dev/null 2>/dev/null |
            grep -E '^#define __ARM_(ARCH|FEATURE)' |
            grep -v '__ARM_FEATURE_FMA ' | LC_ALL=C sort > "$tmpa"
        clang --target=$t "$@" -ffreestanding -E -dM - < /dev/null 2>/dev/null |
            grep -E '^#define __ARM_(ARCH|FEATURE)' |
            grep -v '__ARM_FEATURE_FMA ' | LC_ALL=C sort > "$tmpb"
        # (Baseline: the five clang claims and the core lacks, as above)
        case $t in thumbv8m.base*)
            grep -Ev '__ARM_FEATURE_(CLZ|QBIT|SAT|NUMERIC_MAXMIN|DIRECTED_ROUNDING) ' \
                "$tmpb" > "$tmpb.x"; mv "$tmpb.x" "$tmpb" ;;
        esac
        if [ ! -s "$tmpb" ] || ! cmp -s "$tmpa" "$tmpb"; then
            echo "--target=$t $*: the ARM macros disagree with clang (< clang, > EmbCC):"
            diff "$tmpb" "$tmpa" | head -10
            rm -f "$tmpa" "$tmpb"; exit 1
        fi
        ncm=$((ncm + 1))
    done
    rm -f "$tmpa" "$tmpb"
    echo "Cortex-M -mcpu/-march: $ncm combinations' __ARM_ARCH*/__ARM_FEATURE_* match clang"
fi

# -mcmse, the Secure side of ARMv8-M: __ARM_FEATURE_CMSE is 3 where the table
# says 1, as clang defines it under the flag, at both profiles -- and the rest
# of the table is the same.
tmpa=${TMPDIR:-/tmp}/predef.nocmse.$$
tmpb=${TMPDIR:-/tmp}/predef.cmse.$$
for triple in thumbv8m.main-none-eabi thumbv8m.base-none-eabi; do
    "$EMBCC" --target=$triple --dump-predef | sort > "$tmpa"
    "$EMBCC" --target=$triple -mcmse --dump-predef | sort > "$tmpb"
    d=$(diff "$tmpa" "$tmpb" | grep '^[<>]')
    rm -f "$tmpa" "$tmpb"
    want='< #define __ARM_FEATURE_CMSE 1
> #define __ARM_FEATURE_CMSE 3'
    [ "$d" = "$want" ] || {
        echo "$triple -mcmse: the table should change in __ARM_FEATURE_CMSE alone:"
        printf '%s\n' "$d" | head -6; exit 1; }
done
echo "-mcmse makes __ARM_FEATURE_CMSE 3 on both ARMv8-M profiles, and changes nothing else"
