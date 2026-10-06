#!/bin/sh
# What the MIPS32 backend COMPUTES: every program in tests/exec, at -O0,
# -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# mipsel and run on QEMU's malta board (tests/harness/mips) -- the exit
# status must be the program's `// expect-exit` value.
#
# The corpus was written for x86-64 and AArch64, both LP64, and a few
# programs assert that: `long` holds 2^40, a pointer is 8 bytes, a struct
# of longs is 16. On ILP32 o32 those programs exit with another value --
# the same one clang's code exits with. So for them the reference is not
# the comment but clang: each is compiled by clang for the same triple
# (-mcpu=mips32r2 -msoft-float), linked with the SAME libc, runtime and
# harness, and run on the same board, and EmbCC's status must equal
# clang's. That keeps them in the test, checking something real, rather
# than skipped.
#
# What cannot run here, and why, is listed in na() below. Every other
# program must pass at every level; a failure prints the program, the
# level and what it printed (an exception prints ==FAULT== with its cause
# and address, from the harness).
set -u
echo "TEST-MARKER mips-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_MIPS:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || CLANG=

T=mipsel-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/mips-exec
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
    int128|int128-atomic|int128-more|int128-narrow-ext|packed-wide-bitfields|helper-clobber)
        echo "__int128, which o32 does not have (as on every 32-bit target)" ;;
    static-assert|kernel-vtable)
        echo "static assertions of the LP64 sizes" ;;
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    bitfields)
        echo "a 40-bit field of an unsigned long, which is 32 bits here" ;;
    bit-builtins|global-aggregates|longs)
        echo "shifts a long by 32 or more, undefined when long is 32 bits" ;;
    alignas-decl)
        echo "a 16-aligned scalar local, refused by name above the 8-byte o32 stack (as on Thumb)" ;;
    computed-goto)
        echo "computed goto, refused by name (tests/golden/mips-refuse.sh)" ;;
    atomics|atomics-reg)
        echo "1- and 2-byte atomics and __builtin_frame_address, refused by name (mips-refuse.sh)" ;;
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

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD CC -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; cc=$6; d=$(dirname "$o")
if [ "$cc" = clang ]; then
    "$CLANG" --target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float $opt \
        -ffreestanding -isystem lib/libc/include -w -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
else
    "$embcc" --target=mipsel-none-elf $opt -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
fi
"$embld" -e _start -Ttext 0x80100000 -Tstack 0x80800000 "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
sh tests/harness/mips/run.sh "$o.elf" > "$o.out" 2>&1
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE
export CLANG

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
    if lp64 "$n"; then
        if [ -z "$CLANG" ]; then
            echo "n/a $n: assumes LP64, and there is no clang to referee it" \
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
        if lp64 "$n"; then
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
    echo "mips $opt: $p of $t programs pass on the board"
done
echo "($nlp of them refereed against clang's status for an LP64 assumption;"
echo " $nna not applicable to an ILP32 target, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on MIPS32 at -O0, -O1, -O2 and -Os"
