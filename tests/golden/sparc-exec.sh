#!/bin/sh
# What the SPARC backend COMPUTES: every program in tests/exec, at -O0,
# -O1, -O2 and -Os, compiled for sparc-none-elf, linked by embld with
# lib/libc and lib/rt built for it and run on QEMU's leon3_generic board
# (tests/harness/sparc) -- the exit status must be the program's
# `// expect-exit` value. The harness installs the window overflow and
# underflow trap handlers, so every deep call chain here exercises them.
#
# The corpus was written for x86-64 and AArch64, both LP64 and
# little-endian, and a few programs assert that: `long` holds 2^40, a
# pointer is 8 bytes, a union is read through another member. On SPARC
# (ILP32, big-endian) those programs exit with another value -- the same
# one clang's code exits with. So for them the reference is clang: each is
# compiled by clang for sparc-none-elf -mcpu=leon3 -msoft-float, linked
# with the SAME libc, runtime and harness, run on the same board, and
# EmbCC's status must equal clang's.
#
# tests/golden/sparc-exec/*.c are SPARC's own programs, run the same way:
# long-double-quad.c is tests/exec/long-double.c for an ILP32 machine whose
# long double is an 8-aligned binary128.
#
# What cannot run here, and why, is listed in na() below. Every other
# program must pass at every level; a failure prints the program, the
# level and what it printed (a trap prints ==FAULT tt pc==).
set -u
NAME=sparc-exec T=sparc-none-elf CT=sparc-none-elf
QEMU=${EMBCC_QEMU_SPARC:-qemu-system-sparc}
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_SPARC:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT -mcpu=leon3 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || CLANG=

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out/run"

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/sparc/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/sparc/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    int128|int128-atomic|int128-more|int128-narrow-ext|packed-wide-bitfields|helper-clobber)
        echo "__int128, which 32-bit SPARC does not have (as on every 32-bit target)" ;;
    static-assert|kernel-vtable)
        echo "static assertions of the LP64 sizes" ;;
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    bitfields)
        echo "a 40-bit field of an unsigned long, which is 32 bits here" ;;
    bit-builtins|global-aggregates|longs)
        echo "shifts a long by 32 or more, undefined when long is 32 bits" ;;
    alignas-decl)
        echo "a 16-aligned scalar local, refused by name above the 8-byte SPARC stack (as on Thumb)" ;;
    atomics)
        echo "__builtin_frame_address(1), refused by name: no frame chain (sparc-refuse.sh)" ;;
    atomics-reg)
        echo "a 64-bit long (0x123456789abcdef0L in a long: it exits 4 on ILP32); its 1- and 2-byte atomics run in sparc-atomics.sh" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    *) return 1 ;;
    esac
}

# The programs whose expect-exit assumes LP64, judged against clang.
lp64() {
    case $1 in
    attr-layout|c-extras2|complex|enum-wide-values|ext-add|globals|\
    gnu-attr-positions|long-double|sizeof-cast|static-local-init|strings|\
    structs|u64-float) return 0 ;;
    *) return 1 ;;
    esac
}

# The programs whose expect-exit assumes LITTLE-endian byte order -- a
# union read through another member, a word's first byte, a double's
# words through a {lsw, msw} struct. Big-endian they exit with another
# value, the one clang's code exits with, so there they are judged against
# clang as the LP64 ones are. Each was read for the assumption, and
# tests/exec/endian.c checks the same things in either order.
le_order() {
    case $1 in
    c11-niceties|ext-index|fold-float-constants|fp-bits|packed-bitfields|\
    pragma-pack|ro-globals|store-forward) return 0 ;;
    *) return 1 ;;
    esac
}
refd() { lp64 "$1" || le_order "$1"; }

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD CC -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; cc=$6; d=$(dirname "$o")
if [ "$cc" = clang ]; then
    "$CLANG" --target=$CT -mcpu=leon3 -msoft-float $opt \
        -ffreestanding -isystem lib/libc/include -w -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
else
    "$embcc" --target=$T $opt -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
fi
"$embld" -e _start -Ttext 0x40000000 -Tstack 0x40800000 "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
sh tests/harness/sparc/run.sh "$o.elf" > "$o.out" 2>&1
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE
export CLANG T CT

: > "$out/jobs"
nna=0; nlp=0
for c in tests/exec/*.c tests/golden/sparc-exec/*.c; do
    n=$(basename "$c" .c)
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$c" | head -1)
    [ -n "$only" ] && [ "$only" != $T ] && continue
    if why=$(na "$n"); then
        echo "n/a $n: $why" >> "$out/na.txt"
        nna=$((nna + 1))
        continue
    fi
    if refd "$n"; then
        if [ -z "$CLANG" ]; then
            echo "n/a $n: assumes LP64 or little-endian, and there is no clang to referee it" \
                >> "$out/na.txt"
            nna=$((nna + 1))
            continue
        fi
        echo "$c -O1 $out/run/$n-clang clang" >> "$out/jobs"
        nlp=$((nlp + 1))
    fi
    for opt in -O0 -O1 -O2 -Os; do
        echo "$c $opt $out/run/$n$opt embcc" >> "$out/jobs"
    done
done
jobs=${EMBCC_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
ONE="$out/one.sh" EMBCC_X="$EMBCC" EMBLD_X="$EMBLD"
export ONE EMBCC_X EMBLD_X
# shellcheck disable=SC2016
xargs -P "$jobs" -n 4 sh -c \
    'sh "$ONE" "$0" "$1" "$2" "$EMBCC_X" "$EMBLD_X" "$3" > "$2.status"' \
    < "$out/jobs"
fail=0
for opt in -O0 -O1 -O2 -Os; do
    p=0; t=0
    for c in tests/exec/*.c tests/golden/sparc-exec/*.c; do
        n=$(basename "$c" .c)
        f="$out/run/$n$opt.status"
        [ -f "$f" ] || continue
        t=$((t + 1))
        got=$(cat "$f")
        if refd "$n"; then
            want=$(cat "$out/run/$n-clang.status")
        else
            want=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" |
                   head -1)
        fi
        if [ "$got" = "$want" ]; then
            p=$((p + 1))
        else
            echo "FAIL $n $opt: got $got, want $want"
            head -c 600 "$out/run/$n$opt.cerr" "$out/run/$n$opt.lerr" \
                "$out/run/$n$opt.out" 2>/dev/null | sed 's/^/    /'
            fail=1
        fi
    done
    echo "$NAME $opt: $p of $t programs pass on the board"
done
echo "($nlp of them refereed against clang's status for an LP64 or little-endian assumption;"
echo " $nna not applicable to an ILP32 target, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on SPARC V8 ($T) at -O0, -O1, -O2 and -Os"
