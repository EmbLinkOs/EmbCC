#!/bin/sh
# The seventh parameter, and the six that arrive in registers.
#
# SysV passes six integer arguments in rdi, rsi, rdx, rcx, r8 and r9;
# the seventh onward arrive on the caller's stack. At -O2 a parameter
# may live in a register of its own, so the prologue has two jobs: move
# the six incoming ones to wherever the allocator put them, and load the
# stack-passed ones from the caller's frame.
#
# The order between those two jobs is the whole of this test. r8 and r9
# are in the allocator's pool, so the SEVENTH parameter's home may be a
# register that still holds the FIFTH or SIXTH. Loading it first
# destroys the incoming value before the shuffle reads it, and the
# function then computes with the wrong argument -- silently, and only
# at -O2, and only when the allocator happens to choose that register.
#
# So the case sweeps parameter counts, types and use orders: what
# decides the allocation is which parameter is read first, and there is
# no way to ask for a particular assignment from the outside. 25 of the
# 48 shapes below were wrong before the prologue deferred those loads.
set -eu
echo "TEST-MARKER stack-args"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/stack-args
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: the stack-argument boundary here is SysV x86-64"; exit 0; }

# ---- build the sweep ----------------------------------------------------
#
# For each parameter count, each type mix, and each order of use: a
# function summing p_i * (i+1), which is the same value however the
# terms are written, so one expected answer covers every order.
{
    echo '#include <stdio.h>'
    n=7
    while [ "$n" -le 10 ]; do
        for mix in all_long all_char last_char mixed; do
            for ord in fwd rev odd; do
                nm="f_${n}_${mix}_${ord}"
                # the parameter list
                ps=""; i=0
                while [ "$i" -lt "$n" ]; do
                    case $mix in
                    all_long)  t=long ;;
                    all_char)  t=char ;;
                    last_char) if [ "$i" = "$((n-1))" ]; then t=char; else t=long; fi ;;
                    *)         if [ "$i" = "$((n-1))" ]; then t=long
                               elif [ "$i" = "$((n-2))" ]; then t=char
                               else t=int; fi ;;
                    esac
                    [ -z "$ps" ] || ps="$ps, "
                    ps="$ps$t p$i"
                    i=$((i + 1))
                done
                # the order the terms are written in
                terms=""; i=0
                while [ "$i" -lt "$n" ]; do
                    case $ord in
                    fwd) k=$i ;;
                    rev) k=$((n - 1 - i)) ;;
                    *)   if [ $((i % 2)) = 0 ]; then k=$((i / 2))
                         else k=$((n - 1 - i / 2)); fi ;;
                    esac
                    [ -z "$terms" ] || terms="$terms + "
                    terms="$terms(long)p$k*$((k + 1))"
                    i=$((i + 1))
                done
                echo "static long $nm($ps) { return $terms; }"
            done
        done
        n=$((n + 1))
    done
    echo 'int main(void)'
    echo '{'
    echo '    unsigned long h = 0;'
    n=7
    while [ "$n" -le 10 ]; do
        # p_i = 7 + 3i, so every argument is distinct and fits a char
        args=""; i=0; want=0
        while [ "$i" -lt "$n" ]; do
            v=$((7 + 3 * i))
            [ -z "$args" ] || args="$args, "
            args="$args$v"
            want=$((want + v * (i + 1)))
            i=$((i + 1))
        done
        for mix in all_long all_char last_char mixed; do
            for ord in fwd rev odd; do
                nm="f_${n}_${mix}_${ord}"
                echo "    if ($nm($args) != $want) {"
                echo "        printf(\"$nm: %ld, want $want\", $nm($args));"
                echo '        putchar(10);'
                echo '        return 1;'
                echo '    }'
                echo "    h = h*1000003u + (unsigned long)$nm($args);"
            done
        done
        n=$((n + 1))
    done
    echo '    printf("%lu", h); putchar(10);'
    echo '    return 42;'
    echo '}'
} > "$out/a.c"

nfn=$(grep -c '^static long f_' "$out/a.c")
[ "$nfn" -ge 40 ] || { echo "FAIL: only $nfn shapes generated"; exit 1; }

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    prev=
    for O in 0 1 2; do
        "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/a.c" -o "$out/r" \
            2> "$out/cc.log" || { echo "FAIL: build at -O$O:"
                                  cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || {
            echo "FAIL: exited $rc at -O$O -- a parameter reached the body"
            echo "      holding another parameter's value:"
            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: answers $ans at -O$O where a lower level said $prev."
            exit 1; }
        prev=$ans
    done
    echo "$nfn parameter shapes agree at -O0, -O1 and -O2"

    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/a.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/g.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the sweep"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" \
            > "$out/g.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
        [ "$(cat "$out/g.txt")" = "$prev" ] || {
            echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
            exit 1; }
        echo "and the same answer as gcc"
    fi
else
    echo "(not run: needs a kernel and a libc for tests/harness/linux)"
fi
