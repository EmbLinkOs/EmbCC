#!/bin/sh
# Every runtime routine each embedded backend can call is in that target's
# librt.a -- exactly once.
#
# This is the test that did not exist, and its absence had a cost. The
# ARMv8-M table claimed a hardware FPU (`__ARM_FP 0xe`), so lib/rt/softfp.c's
# guard compiled it to an EMPTY object for that target, while the backend --
# which does every float operation as a call -- went on emitting __addsf3. No
# float program linked for a Cortex-M33, and nothing said so until someone
# tried to link one. Separately, lib/rt/rt.h named __int128 unconditionally,
# so lib/rt/complex.c built for no embedded target at all and `float _Complex`
# multiply had no runtime anywhere; and on AVR, lib/rt/int64.c and
# lib/rt/avr64.c BOTH defined __udivdi3, so which one a program got depended
# on the order the archive happened to be built in.
#
# All three are one question -- does the archive hold what the backend calls,
# once? -- and this asks it without a board, a harness or QEMU, in a couple of
# seconds, for every embedded target.
#
# The list of names comes from the BACKEND'S OWN SOURCE: every string literal
# in src/arch/<backend>/*.c that is spelled like a libgcc helper, plus the
# complex helpers src/sema/sema.c emits. So a new call site in a backend is
# checked the day it is written, without anyone remembering to add it here.
# The 128-bit helpers are asked of the targets that can reach them, by what
# the target's own predefined macros say it has: the `ti` routines where
# there is an __int128 (RV64), the `tf` routines and the long-double complex
# pair __multc3/__divtc3 where `long double` is binary128 (RV32 and RV64;
# elsewhere it is no wider than `double`), and the 64-bit divides only where
# a register is 32 bits.
set -u
echo "TEST-MARKER embedded-runtime"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/embedded-runtime
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
export EMBCC

pat='^__(float|fix|mul|div|mod|udiv|umod|add|sub|cmp|eq|ne|lt|le|gt|ge|neg|extend|trunc|unord|ashl|ashr|lshr|clz|ctz|popcount|bswap|parity|powi)'

shared=$(grep -ohE '"__(mul|div)(sc|dc)3"' src/sema/sema.c | tr -d '"' | sort -u)
[ -n "$shared" ] || { echo "found no complex helper names in src/sema/sema.c"; exit 1; }

fail=0
checked=0
for triple in avr thumbv6m-none-eabi thumbv7m-none-eabi thumbv7em-none-eabi \
              thumbv8m.main-none-eabi riscv32-unknown-elf riscv64-unknown-elf; do
    case $triple in
        avr)     be=avr ;;
        thumb*)  be=thumb ;;
        riscv*)  be=riscv ;;
    esac
    d=$out/$triple
    sh tools/build-rt.sh "$triple" "$d" 2> "$d.err" || {
        echo "$triple: the runtime does not build:"; head -3 "$d.err"
        fail=1; continue; }

    llvm-nm --defined-only "$d/librt.a" 2>/dev/null \
        | awk '$2 == "T" { print $3 }' | sort > "$d/defined"
    # ARMv6-M's routines are weak (a program's own wins), and count as there.
    llvm-nm --defined-only "$d/librt.a" 2>/dev/null \
        | awk '$2 == "T" || $2 == "W" { print $3 }' | sort -u > "$d/present"

    # Defined twice is a failure too: in an archive the first member wins, so
    # a duplicate means the routine a program gets depends on build order.
    dups=$(uniq -d "$d/defined")
    once=", once each"
    if [ -n "$dups" ]; then
        once=""                    # and do not claim it below
        echo "$triple: defined by more than one member of librt.a:"
        echo "$dups" | sed 's/^/    /'
        fail=1
    fi

    pd=$("$EMBCC" --target="$triple" --dump-predef)
    drop='^$'; wide=""
    echo "$pd" | grep -q __SIZEOF_INT128__ ||
        drop="$drop|^__([a-z]*ti[0-9]|float(un)?ti[a-z]+|fix(uns)?[a-z]+ti)\$"
    if echo "$pd" | grep -q '__LDBL_MANT_DIG__ 113'
    then wide="__multc3 __divtc3"
    else drop="$drop|tf"
    fi
    echo "$pd" | grep -q '__SIZEOF_POINTER__ 8' &&
        drop="$drop|^__u?(div|mod)di3\$"
    # ARMv6-M's lowering also calls the RTABI and libatomic names
    # (src/arch/thumb/v6m.c), which only that level's runtime defines.
    v6=
    [ "$triple" = thumbv6m-none-eabi ] &&
        v6=$(grep -ohE '"__(aeabi|atomic|sync)_[a-z0-9_]+"' src/arch/thumb/v6m.c |
             tr -d '"')
    names=$( { grep -ohE '"__[a-z0-9]+"' src/arch/$be/*.c | tr -d '"' \
                 | grep -E "$pat" | grep -vE "$drop"; echo "$shared"
               for w in $wide $v6; do echo "$w"; done; } | sort -u)
    missing=
    for nm in $names; do
        checked=$((checked + 1))
        grep -qx "$nm" "$d/present" || missing="$missing $nm"
    done
    if [ -n "$missing" ]; then
        echo "$triple: the $be backend can call these and librt.a does not define them:"
        for nm in $missing; do echo "    $nm"; done
        fail=1
    else
        n=$(echo "$names" | wc -l | tr -d ' ')
        echo "$triple: all $n routines the $be backend can call are in librt.a$once"
    fi
done

[ "$fail" -eq 0 ] || exit 1
echo "every embedded target's runtime is complete: $checked (target, routine)
pairs checked against the names the backends themselves emit, and no routine
is defined twice"
