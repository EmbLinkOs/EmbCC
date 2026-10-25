#!/bin/sh
# What the ColdFire backend COMPUTES: every program in tests/exec, at -O0,
# -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# m68k-none-elf and run on QEMU's mcf5208evb (tests/harness/coldfire) --
# the exit status must be the program's `// expect-exit` value.
#
# The corpus was written for x86-64 and AArch64, LP64 and little-endian,
# and a few programs assert that. There is no m68k compiler here to say
# what they exit with on this target, so they are refereed against the
# nearest one there is: clang for mips-unknown-elf -- ILP32, big-endian, a
# signed char, long double = double, as m68k-none-elf -- compiled with the
# same libc and run on QEMU's big-endian malta (tests/harness/mips). Where
# the m68k's 2-byte alignment changes a program's answer from that one,
# the program is judged against the value the m68k layout gives, written
# down below with the reason (m68k_value).
#
# What cannot run here, and why, is listed in na() below. Every other
# program must pass at every level; a failure prints the program, the
# level and what it printed (an exception prints ==FAULT== with its
# vector and pc, from the harness).
set -u
NAME=coldfire-exec T=m68k-none-elf
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_M68K:-qemu-system-m68k}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_MIPS:-clang}
RT=mips-none-elf CT=mips-unknown-elf
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT -mcpu=mips32r2 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null &&
    command -v qemu-system-mips >/dev/null 2>&1 || CLANG=

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out/run"

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/coldfire/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/coldfire/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }
if [ -n "$CLANG" ]; then
    mkdir -p "$out/ref"
    EMBCC="$EMBCC" sh tools/build-rt.sh $RT "$out/ref/lib" &&
    EMBCC="$EMBCC" sh tools/build-libc.sh $RT "$out/ref/lib" &&
    "$EMBCC" --target=$RT -O1 -DHARNESS_LIBC -c tests/harness/mips/boot.c \
        -o "$out/ref/boot.o" &&
    "$EMBCC" --target=$RT -O1 -c tests/harness/mips/io.c -o "$out/ref/io.o" ||
        { echo "the big-endian MIPS reference does not build"; exit 1; }
fi

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    int128|int128-atomic|int128-more|int128-narrow-ext|packed-wide-bitfields|helper-clobber)
        echo "__int128, which the m68k does not have (as on every 32-bit target)" ;;
    static-assert|kernel-vtable)
        echo "static assertions of the LP64 sizes" ;;
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    bitfields)
        echo "a 40-bit field of an unsigned long, which is 32 bits here" ;;
    bit-builtins|global-aggregates|longs)
        echo "shifts a long by 32 or more, undefined when long is 32 bits" ;;
    alignas-decl|alignof-object)
        echo "a 16- or 8-aligned scalar local, refused by name above the 4-byte stack" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    *) return 1 ;;
    esac
}

# The programs whose expect-exit assumes LP64 or little-endian order,
# judged against clang's big-endian ILP32 MIPS (mips-exec.sh's lists).
refd() {
    case $1 in
    attr-layout|c-extras2|complex|enum-wide-values|ext-add|globals|\
    gnu-attr-positions|long-double|sizeof-cast|static-local-init|strings|\
    structs|u64-float) return 0 ;;
    c11-niceties|ext-index|fold-float-constants|fp-bits|packed-bitfields|\
    pragma-pack|ro-globals|store-forward) return 0 ;;
    atomics-reg) return 0 ;;      # a long holding 0x123456789abcdef0
    *) return 1 ;;
    esac
}

# The programs whose answer the m68k's 2-byte alignment makes differ from
# both the comment and big-endian MIPS: the value on this target, and why.
m68k_value() {
    case $1 in
    anon-struct-typename)
        # sizeof(struct { char c; int i; }) is 6 and offsetof(struct
        # { char c; long l; }, l) 2: 6 + 2 + 2 + 32 - 4
        echo "38 int and long are 2-aligned" ;;
    *) return 1 ;;
    esac
}

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD CC -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; cc=$6; d=$(dirname "$o")
if [ "$cc" = clang ]; then
    "$CLANG" --target=$CT -mcpu=mips32r2 -msoft-float $opt \
        -ffreestanding -isystem lib/libc/include -w -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
    "$embld" -e _start -Ttext 0x80100000 -Tstack 0x80800000 \
        "$d/../ref/boot.o" "$d/../ref/io.o" "$o.o" "$d/../ref/lib/libc.a" \
        "$d/../ref/lib/librt.a" -o "$o.elf" > "$o.lerr" 2>&1 ||
        { echo LFAIL; exit 0; }
    sh tests/harness/mips/run.sh "$o.elf" > "$o.out" 2>&1
else
    "$embcc" --target=$T $opt -c "$src" -o "$o.o" \
        > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
    "$embld" -e _start -Ttext 0x40100000 -Tstack 0x48000000 "$d/../boot.o" \
        "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
        -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
    sh tests/harness/coldfire/run.sh "$o.elf" > "$o.out" 2>&1
fi
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE
export CLANG T CT

: > "$out/jobs"
nna=0; nref=0
for c in tests/exec/*.c; do
    n=$(basename "$c" .c)
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$c" | head -1)
    [ -n "$only" ] && [ "$only" != $T ] && continue
    if why=$(na "$n"); then
        echo "n/a $n: $why" >> "$out/na.txt"
        nna=$((nna + 1))
        continue
    fi
    if refd "$n" && ! m68k_value "$n" >/dev/null; then
        if [ -z "$CLANG" ]; then
            echo "n/a $n: assumes LP64 or little-endian, and there is no clang to referee it" \
                >> "$out/na.txt"
            nna=$((nna + 1))
            continue
        fi
        echo "$c -O1 $out/run/$n-clang clang" >> "$out/jobs"
        nref=$((nref + 1))
    fi
    for opt in ${CF_EXEC_OPTS:--O0 -O1 -O2 -Os}; do
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
for opt in ${CF_EXEC_OPTS:--O0 -O1 -O2 -Os}; do
    p=0; t=0
    for c in tests/exec/*.c; do
        n=$(basename "$c" .c)
        f="$out/run/$n$opt.status"
        [ -f "$f" ] || continue
        t=$((t + 1))
        got=$(cat "$f")
        if want=$(m68k_value "$n"); then
            want=${want%% *}
        elif refd "$n"; then
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
echo "($nref of them refereed against clang's big-endian MIPS32 status for an"
echo " LP64 or little-endian assumption; $nna not applicable, listed in"
echo " $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on ColdFire ($T) at -O0, -O1, -O2 and -Os"
