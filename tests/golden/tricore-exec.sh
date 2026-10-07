#!/bin/sh
# What the TriCore backend COMPUTES: every program in tests/exec, at -O0,
# -O1, -O2 and -Os, linked by embld with lib/libc and lib/rt built for
# tricore-none-elf and run on QEMU's tricore_testboard (tests/harness/
# tricore) -- the exit status must be the program's `// expect-exit`
# value.
#
# The corpus was written for x86-64 and AArch64, both LP64, and a few
# programs assert that: `long` holds 2^40, a pointer is 8 bytes, a struct
# of longs is 16. There is no TriCore reference compiler here to run them
# against, so for those the expected status is written below (lp64_want),
# each worked out from the program for an ILP32 target whose long long
# and double are 4-aligned, signed char, long double = double -- the
# TriCore EABI's model -- and each agreeing with what clang's MIPS32 code
# returns for the same program wherever the 8-byte alignment does not
# enter into it.
#
# What cannot run here, and why, is listed in na() below. Every other
# program must pass at every level; a failure prints the program, the
# level and what it printed.
set -u
echo "TEST-MARKER tricore-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }

T=tricore-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/tricore-exec
rm -rf "$out"; mkdir -p "$out/run"

inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$out/putc.so" \
   tests/harness/tricore/putc.c 2>/dev/null ||
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -o "$out/putc.so" tests/harness/tricore/putc.c ||
    { echo "the harness's output plugin does not build"; exit 1; }
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
export EMBCC_TRICORE_PLUGIN

EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" ||
    { echo "lib/libc does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/tricore/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# The programs that do not apply to this target, each with the reason. A
# program here is not run; one that should be belongs in the corpus run.
na() {
    case $1 in
    int128|int128-atomic|int128-more|int128-narrow-ext|packed-wide-bitfields|helper-clobber)
        echo "__int128, which TriCore does not have (as on every 32-bit target)" ;;
    static-assert|kernel-vtable)
        echo "static assertions of the LP64 sizes" ;;
    preprocessor)
        echo "#errors unless the target is x86-64 or AArch64" ;;
    bitfields)
        echo "a 40-bit field of an unsigned long, which is 32 bits here" ;;
    bit-builtins|global-aggregates|longs)
        echo "shifts a long by 32 or more, undefined when long is 32 bits" ;;
    alignas-decl)
        echo "a 16-aligned scalar local, refused by name above the 8-byte stack (as on Thumb and MIPS)" ;;
    computed-goto|computed-goto-more)
        echo "computed goto, refused by name (tests/golden/tricore-refuse.sh)" ;;
    atomics|atomics-reg)
        echo "1- and 2-byte atomics and __builtin_frame_address, refused by name (tricore-refuse.sh)" ;;
    volatile-local-longjmp)
        echo "setjmp/longjmp, which lib/libc implements for x86-64 and AArch64 only" ;;
    *) return 1 ;;
    esac
}

# The programs whose expect-exit assumes LP64, and their status on an
# ILP32 target with TriCore's data model (see the head of this file).
#
# Each program stops at its first failing check, so the status names the
# check: the first that assumes LP64. Every one below is the status clang's
# code for mipsel (-mcpu=mips32r2 -msoft-float, ILP32 with a signed char
# and long double = double) returns on the malta board -- the referee
# tests/golden/mips-exec.sh uses -- and the 4-aligned long long changes
# none of them: the first failing check comes before any layout that
# holds one.
lp64_want() {
    case $1 in
    attr-layout|c-extras2|complex|long-double|structs) echo 1 ;;
    gnu-attr-positions) echo 2 ;;
    static-local-init|u64-float) echo 3 ;;
    globals|strings) echo 4 ;;
    enum-wide-values) echo 5 ;;
    sizeof-cast) echo 8 ;;
    ext-add) echo 48 ;;
    *) return 1 ;;
    esac
}

# One run: compile, link, boot, and the exit status the sentinel reports.
cat > "$out/one.sh" <<'ONE'
# one.sh SRC OPT OUT EMBCC EMBLD -- prints "<status>" or "CFAIL"/"LFAIL"/"NOEXIT"
src=$1; opt=$2; o=$3; embcc=$4; embld=$5; d=$(dirname "$o")
"$embcc" --target=tricore-none-elf $opt -c "$src" -o "$o.o" \
    > "$o.cerr" 2>&1 || { echo CFAIL; exit 0; }
"$embld" -e _start -Ttext 0x80000000 -Tdata 0xa1000000 -Tstack 0xa1400000 \
    --csa 0x80180000:0x80200000 "$d/../boot.o" \
    "$d/../io.o" "$o.o" "$d/../lib/libc.a" "$d/../lib/librt.a" \
    -o "$o.elf" > "$o.lerr" 2>&1 || { echo LFAIL; exit 0; }
sh tests/harness/tricore/run.sh "$o.elf" > "$o.out" 2>&1
s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$o.out" | tail -1)
echo "${s:-NOEXIT}"
ONE

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
    lp64_want "$n" >/dev/null && nlp=$((nlp + 1))
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
        if ! want=$(lp64_want "$n"); then
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
    echo "tricore $opt: $p of $t programs pass on the board"
done
echo "($nlp of them judged against their ILP32 status for an LP64 assumption;"
echo " $nna not applicable, listed in $out/na.txt)"
[ "$fail" = 0 ] || exit 1
echo "the exec corpus runs on TriCore at -O0, -O1, -O2 and -Os"
