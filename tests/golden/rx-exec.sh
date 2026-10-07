#!/bin/sh
# What the Renesas RX backend COMPUTES: every program in tests/exec, at
# -O0, -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# rx-none-elf and run on QEMU's gdbsim-r5f562n8 board (tests/harness/rx)
# -- the exit status must be the program's `// expect-exit` value.
#
# The corpus was written for x86-64 and AArch64: LP64, and a 64-bit
# double. RX as GCC's rx-elf configures it is ILP32 with a BINARY32 double
# (docs/internals/rx-plan.md), so a program whose `expect-exit` assumes
# either exits with another value on RX -- the value GCC's own code
# computes. For those the reference is rxref() below: the status each
# exits with when compiled by rx-elf-gcc 16.2.0 -nofpu -O1, linked with
# the SAME lib/libc and harness and run on the same board, recorded on
# 2026-10-07 (there is no RX compiler here to ask each time). That keeps
# them in the test, checking something real, rather than skipped.
#
# What cannot run here, and why, is listed in na(). Every other program
# must pass at every level; a failure prints the program, the level and
# what it printed (a fault prints ==FAULT== from the harness).
set -u
echo "TEST-MARKER rx-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
OBJCOPY=${EMBCC_OBJCOPY:-llvm-objcopy}
command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "skipped: $OBJCOPY not found (set EMBCC_OBJCOPY)"; exit 0; }
export EMBCC_OBJCOPY="$OBJCOPY"

T=rx-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/rx-exec
rm -rf "$out"; mkdir -p "$out/run"

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/rx/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/rx/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    int128|int128-atomic|int128-more|int128-narrow-ext|packed-wide-bitfields|helper-clobber)
        echo "__int128, which RX does not have (as on every 32-bit target)" ;;
    static-assert|kernel-vtable)
        echo "static assertions of the LP64 sizes" ;;
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    computed-goto)
        echo "computed goto, refused by name (tests/golden/rx-refuse.sh)" ;;
    bitfields)
        echo "a 40-bit field of an unsigned long, which is 32 bits here" ;;
    bit-builtins|global-aggregates|longs)
        echo "shifts a long by 32 or more, undefined when long is 32 bits" ;;
    alignas-decl|alignof-object)
        echo "a scalar local aligned beyond the 4-byte stack, refused by name (as on Thumb and MIPS)" ;;
    atomics)
        echo "__builtin_frame_address, refused by name (rx-refuse.sh)" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    *) return 1 ;;
    esac
}

# The programs whose expect-exit assumes LP64 or a binary64 double, and
# the status rx-elf-gcc 16.2.0 -nofpu's code exits with on this board.
rxref() {
    case $1 in
    attr-layout|c-extras2|complex|fp-bits|long-double|stdc-version|structs)
        echo 1 ;;
    frontend-gaps|gnu-attr-positions) echo 2 ;;
    float-truth|static-local-init|u64-float) echo 3 ;;
    atomics-reg|globals|strings|vla) echo 4 ;;
    enum-wide-values|store-forward) echo 5 ;;
    floats) echo 6 ;;
    sizeof-cast) echo 8 ;;
    int64-to-float) echo 10 ;;
    fp-bits-long-double) echo 13 ;;
    ext-add) echo 48 ;;
    *) return 1 ;;
    esac
}

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; d=$(dirname "$o")
"$embcc" --target=rx-none-elf $opt -c "$src" -o "$o.o" \
    > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
"$embld" -e _start -Ttext 0x01800000 -Tstack 0x02000000 "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 &&
    "$EMBCC_OBJCOPY" -O binary "$o.elf" "$o.bin" >> "$o.lerr" 2>&1 ||
    { echo LFAIL; exit 0; }
sh tests/harness/rx/run.sh "$o.bin" > "$o.out" 2>&1
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
    for opt in ${EMBCC_RX_OPTS:--O0 -O1 -O2 -Os}; do
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
for opt in ${EMBCC_RX_OPTS:--O0 -O1 -O2 -Os}; do
    p=0; t=0
    for c in tests/exec/*.c; do
        n=$(basename "$c" .c)
        f="$out/run/$n$opt.status"
        [ -f "$f" ] || continue
        t=$((t + 1))
        got=$(cat "$f")
        if want=$(rxref "$n"); then
            :
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
    echo "rx $opt: $p of $t programs pass on the board"
done
nref=0
for c in tests/exec/*.c; do rxref "$(basename "$c" .c)" >/dev/null && nref=$((nref + 1)); done
echo "($nref of them judged by the status rx-elf-gcc's code exits with, for an"
echo " LP64 or binary64 assumption; $nna not applicable, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on RX at -O0, -O1, -O2 and -Os"
