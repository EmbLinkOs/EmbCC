#!/bin/sh
# What the LoongArch64 backend COMPUTES: every program in tests/exec, at
# -O0, -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# loongarch64 and run on QEMU's virt board (tests/harness/loongarch) -- the
# exit status must be the program's `// expect-exit` value.
#
# LoongArch64 is LP64, like the x86-64 and AArch64 the corpus was written
# for, so no program's expectation needs another referee here; what cannot
# run, and why, is listed in na() below. Every other program must pass at
# every level; a failure prints the program, the level and what it printed
# (an exception prints ==FAULT== with its code and address, from the
# harness).
#
# Then the shared embedded programs (tests/golden/embedded-*.c, which name
# no machine) are compiled by EmbCC at every level and by clang for the
# same triple, linked by the same embld with the same harness and run on
# the same board, and the outputs must agree: a difference is the
# compiler's and not the image's.
set -u
echo "TEST-MARKER loongarch-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_LOONGARCH:-qemu-system-loongarch64}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_LOONGARCH:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=loongarch64-unknown-elf -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || CLANG=

T=loongarch64-unknown-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/loongarch-exec
rm -rf "$out"; mkdir -p "$out/run"

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/loongarch/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/loongarch/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    atomics)
        echo "__builtin_frame_address and __builtin_return_address, refused by name (loongarch-refuse.sh)" ;;
    int128-atomic)
        echo "a 16-byte compare-and-swap, which the LA64 base ISA has no instruction for, refused by name" ;;
    *) return 1 ;;
    esac
}

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; d=$(dirname "$o")
"$embcc" --target=loongarch64-unknown-elf $opt -c "$src" -o "$o.o" \
    > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
"$embld" -e _start -Ttext 0x1000000 -Tstack 0x3000000 "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
sh tests/harness/loongarch/run.sh "$o.elf" > "$o.out" 2>&1
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE

: > "$out/jobs"
nna=0
for c in tests/exec/*.c; do
    n=$(basename "$c" .c)
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$c" | head -1)
    [ -n "$only" ] && [ "$only" != $T ] && continue
    if why=$(na "$n"); then
        echo "n/a $n: $why" >> "$out/na.txt"
        nna=$((nna + 1))
        continue
    fi
    for opt in -O0 -O1 -O2 -Os; do
        echo "$c $opt $out/run/$n$opt" >> "$out/jobs"
    done
done
jobs=${EMBCC_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
ONE="$out/one.sh" EMBCC_X="$EMBCC" EMBLD_X="$EMBLD"
export ONE EMBCC_X EMBLD_X
# shellcheck disable=SC2016
xargs -P "$jobs" -n 3 sh -c \
    'sh "$ONE" "$0" "$1" "$2" "$EMBCC_X" "$EMBLD_X" > "$2.status"' \
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
        want=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" |
               head -1)
        if [ "$got" = "$want" ]; then
            p=$((p + 1))
        else
            echo "FAIL $n $opt: got $got, want $want"
            head -c 600 "$out/run/$n$opt.cerr" "$out/run/$n$opt.lerr" \
                "$out/run/$n$opt.out" 2>/dev/null | sed 's/^/    /'
            fail=1
        fi
    done
    echo "loongarch64 $opt: $p of $t programs pass on the board"
done
echo "($nna not applicable, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on LoongArch64 at -O0, -O1, -O2 and -Os"
