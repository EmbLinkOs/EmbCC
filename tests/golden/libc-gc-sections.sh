#!/bin/sh
# lib/libc and librt are built with a section per function and per object
# (tools/build-libc.sh, tools/build-rt.sh), so an image linked with
# --gc-sections carries the functions it reaches and not the rest of the
# files they are in.
#  1. tests/golden/libc-gc-sections/prog.c runs on a Cortex-M4F and an RV32
#     board, linked with and without --gc-sections, and prints the same.
#  2. With --gc-sections, the neighbours it never calls are gone: cosh and
#     hypot (math.c, beside expf), strtold (beside strtof), fprintf and
#     vsprintf (beside printf), fopen (stdio.c, beside stdout).
#  3. The collected image is under half the size of the other.
# Built as one object per file, as the library was, the neighbours stayed:
# printf on the M4 was 16 KB of code where it is 8.
set -u
echo "TEST-MARKER libc-gc-sections"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/libc-gc-sections
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
prog=tests/golden/libc-gc-sections/prog.c
printf 'sorted 12345\nexpf 1.6487 sqrtf 0.7071\nlen 5\n==END==\n' > "$out/want.txt"

ran=0
for t in thumbv7em-none-eabihf riscv32-unknown-elf; do
    case $t in
        thumbv7em*) H=tests/harness/thumb-m4f; hv=EMBCC_THUMB_HARNESS
                    Q="qemu-system-arm -M mps2-an386 -cpu cortex-m4 -nographic" ;;
        riscv32*)   H=tests/harness/riscv; hv=EMBCC_RISCV_HARNESS
                    Q="qemu-system-riscv32 -M virt -bios none -nographic -m 8" ;;
    esac
    L=build/libc/$t
    [ -f $L/libc.a ] && [ -f $L/librt.a ] || fail "no $L/libc.a or librt.a (make libc-embedded rt-embedded)"
    for f in tools/build-libc.sh tools/build-rt.sh; do
        [ $L/libc.a -nt $f ] && [ $L/librt.a -nt $f ] ||
            fail "$L is older than $f (make libc-embedded rt-embedded)"
    done
    d=$out/$t; mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target=$t -c $H/$f.c -o "$d/$f.o" || fail "$t: the harness's $f.c"
    done
    "$EMBCC" --target=$t -Os --sections=functions,data -Ilib/libc/include \
        -c $prog -o "$d/prog.o" || fail "$t: prog.c does not compile"
    for gc in keep gc; do
        a=; [ $gc = gc ] && a=--gc-sections
        # shellcheck disable=SC2086
        env "$hv=$d" sh $H/link.sh "$d/$gc.elf" $a "$d/prog.o" $L/libc.a \
            $L/librt.a > "$d/ld-$gc.txt" 2>&1 ||
            { head -3 "$d/ld-$gc.txt"; fail "$t $gc: does not link"; }
        if command -v "${Q%% *}" >/dev/null 2>&1; then
            # shellcheck disable=SC2086
            sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-60}" --until '==END==' \
                $Q -kernel "$d/$gc.elf" > "$d/$gc.raw" 2>/dev/null
            tr -d '\r' < "$d/$gc.raw" | sed -n '1,/==END==/p' > "$d/$gc.txt"
            cmp -s "$out/want.txt" "$d/$gc.txt" ||
                { diff "$out/want.txt" "$d/$gc.txt" | head; fail "$t $gc: the board prints something else"; }
            ran=$((ran + 1))
        fi
    done
    llvm-nm "$d/gc.elf" > "$d/gc.nm"
    for s in cosh hypot strtold fprintf vsprintf fopen; do
        grep -q " [Tt] $s\$" "$d/gc.nm" && fail "$t: --gc-sections kept $s, which nothing calls"
    done
    for s in expf strtof printf qsort; do
        grep -q " [Tt] $s\$" "$d/gc.nm" || fail "$t: --gc-sections dropped $s, which main calls"
    done
    code() { llvm-size -A "$1" | awk '$1 ~ /^\.text|^\.rodata/ { s += $2 } END { print s }'; }
    k=$(code "$d/keep.elf"); g=$(code "$d/gc.elf")
    [ $((g * 2)) -lt "$k" ] || fail "$t: --gc-sections left $g of $k bytes"
    echo "  $t: $g bytes of code and constants with --gc-sections, $k without"
done
[ $ran -gt 0 ] && how="run on the boards" || how="(no QEMU: linked, not run)"
echo "libc-gc-sections: an image linked with --gc-sections keeps only what it reaches, $how"
