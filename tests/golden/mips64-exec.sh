#!/bin/sh
# What the MIPS64 backend COMPUTES: every program in tests/exec, at -O0,
# -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# mips64el and run on QEMU's malta board with a MIPS64r2 core
# (tests/harness/mips64) -- the exit status must be the program's
# `// expect-exit` value.
#
# n64 is LP64, as the corpus's x86-64 and AArch64 are, so the programs
# that assume LP64 apply here as written; and __int128 exists. A program
# whose expect-exit rests on something n64 does differently -- long double
# is IEEE binary128, not x87's -- is judged instead against clang's build
# of it: compiled by clang for the same triple (-mcpu=mips64r2
# -msoft-float), linked with the SAME libc, runtime and harness, run on the
# same board, and EmbCC's status must equal clang's.
#
# What cannot run here, and why, is listed in na() below. Every other
# program must pass at every level; a failure prints the program, the
# level and what it printed (an exception prints ==FAULT== with its cause
# and address, from the harness).
#
# The same corpus runs BIG-endian (mips64-none-elf, on qemu-system-mips64)
# as tests/golden/mips64-be-exec.sh, which sets MIPS64_EXEC_BE=1 and runs
# this; there the programs that assume little-endian layout are judged
# against clang as the 32-bit big-endian golden judges them.
set -u
if [ "${MIPS64_EXEC_BE:-0}" = 1 ]; then
    NAME=mips64-be-exec T=mips64-none-elf CT=mips64-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS64EB:-qemu-system-mips64}
else
    NAME=mips64-exec T=mips64el-none-elf CT=mips64el-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS64:-qemu-system-mips64el}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_MIPS:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT -mcpu=mips64r2 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || CLANG=

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out/run"

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/mips/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/mips/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    atomics|atomics-reg|int128-atomic)
        echo "1-, 2- and 16-byte atomics and __builtin_frame_address, refused by name (mips64-refuse.sh)" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    packed-wide-bitfields)
        [ "${MIPS64_EXEC_BE:-0}" = 1 ] || return 1
        echo "a packed bit-field across more than 8 bytes, refused by name big-endian" ;;
    *) return 1 ;;
    esac
}

# No program's expect-exit is taken from clang little-endian: n64 is the
# corpus's own LP64, and the one difference that shows -- long double is
# binary128 -- the corpus already allows for (AArch64's is too).
lp64() { return 1; }

# The programs whose expect-exit assumes LITTLE-endian byte order -- a
# union read through another member, a word's first byte, a double's
# words through a {lsw, msw} struct. Big-endian they exit with another
# value, the one clang's code exits with, so there they are judged against
# clang as the LP64 ones are. Each was read for the assumption, and
# tests/exec/endian.c checks the same things in either order.
le_order() {
    [ "${MIPS64_EXEC_BE:-0}" = 1 ] || return 1
    case $1 in
    c11-niceties|ext-index|fold-float-constants|fp-bits|packed-bitfields|\
    pragma-pack|ro-globals|store-forward|structs|gnu-attr-positions) return 0 ;;
    *) return 1 ;;
    esac
}
refd() { lp64 "$1" || le_order "$1"; }

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD CC -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; cc=$6; d=$(dirname "$o")
if [ "$cc" = clang ]; then
    "$CLANG" --target=$CT -mcpu=mips64r2 -msoft-float -mno-abicalls -G0 $opt \
        -ffreestanding -isystem lib/libc/include -w -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
else
    "$embcc" --target=$T $opt -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
fi
"$embld" -e _start -Ttext 0xffffffff80100000 -Tstack 0xffffffff80800000 \
    "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
sh tests/harness/mips64/run.sh "$o.elf" > "$o.out" 2>&1
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE
export CLANG T CT

: > "$out/jobs"
nna=0; nlp=0
for c in tests/exec/*.c; do
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
    for c in tests/exec/*.c; do
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
echo "($nlp of them refereed against clang's status for an x87 or little-endian assumption;"
echo " $nna not applicable, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on MIPS64 ($T) at -O0, -O1, -O2 and -Os"
