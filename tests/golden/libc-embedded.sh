#!/bin/sh
# EmbCC's C library on the boards: RV32, RV64, a Cortex-M3, a Cortex-M4F
# with the hard-float calling convention and a Cortex-M33 (ARMv8-M), each image
# built with lib/libc on its bare-metal backend (tools/build-libc.sh) and
# run under QEMU -- against the SAME library built for x86-64.
#
# The library was compiled for every embedded target and run on none.
# That is how sqrt went on calling itself on all of them, and how a call
# to a weak function bound to the default in its own file, so a program's
# write() was never called and its printf went nowhere. One source on both
# sides means a difference is the compiler's for that target: the program
# (tests/golden/embedded-libc.c) prints every floating-point result with
# %a and avoids what targets may differ in -- `long`, pointers, long double.
set -u
echo "TEST-MARKER libc-embedded"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/libc-embedded
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
export EMBCC
prog=tests/golden/embedded-libc.c

# ---- the reference: x86-64, as tests/golden/libc.sh links a program ----
LIB=build/libc/x86_64/libc.a
GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
command -v x86_64-elf-ld >/dev/null 2>&1 && command -v "$GCC" >/dev/null 2>&1 ||
    { echo "SKIP: no x86_64-elf toolchain for the reference"; exit 0; }
[ -f "$LIB" ] || { echo "SKIP: $LIB absent (make libc-x86_64)"; exit 0; }
LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")
H=tests/harness/x86_64
for part in start sys crt; do
    src=$H/$part.S; [ "$part" = sys ] && src=$H/sys.c
    [ "$part" = crt ] && src=$H/../crt.c
    $GCC -ffreestanding -mno-red-zone -isystem "$X86_NEWLIB/include" \
        -c "$src" -o "$out/x86-$part.o" || { echo "x86 harness: $part"; exit 1; }
done
"$EMBCC" -O1 -Ilib/libc/include -c "$prog" -o "$out/x86.o" || {
    echo "x86-64: the program does not compile"; exit 1; }
x86_64-elf-ld -n -z max-page-size=0x1000 -T "$H/link.ld" -o "$out/x86.64" \
    "$out/x86-start.o" "$out/x86.o" "$out/x86-sys.o" "$out/x86-crt.o" "$LIB" \
    -L"$LIBGCC" -lgcc || { echo "x86-64: the reference does not link"; exit 1; }
x86_64-elf-objcopy -I elf64-x86-64 -O elf32-i386 "$out/x86.64" "$out/x86.elf"
"$H/run.sh" "$out/x86.elf" > "$out/ref.raw" 2>&1
sed -n '1,/==END==/p' "$out/ref.raw" > "$out/ref.txt"
grep -q '==END==' "$out/ref.txt" || {
    echo "x86-64: the reference did not reach the end:"; head -5 "$out/ref.raw"
    exit 1; }

# ---- the boards ---------------------------------------------------------
# Not AVR: a two-byte atomic is two accesses on that part, and lib/libc's
# locks are refused for it.
fail=0
for t in riscv32-unknown-elf riscv64-unknown-elf thumbv7m-none-eabi \
         thumbv7em-none-eabihf thumbv8m.main-none-eabi; do
    case $t in
        riscv32*) H=tests/harness/riscv
                  Q="qemu-system-riscv32 -M virt -bios none -nographic -m 8" ;;
        riscv64*) H=tests/harness/riscv
                  Q="qemu-system-riscv64 -M virt -bios none -nographic -m 8" ;;
        thumbv7m*) H=tests/harness/thumb
                  Q="qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic" ;;
        thumbv7em*) H=tests/harness/thumb-m4f
                  Q="qemu-system-arm -M mps2-an386 -cpu cortex-m4 -nographic" ;;
        thumbv8m*) H=tests/harness/thumb-m33
                  Q="qemu-system-arm -M mps2-an505 -cpu cortex-m33 -nographic" ;;
    esac
    command -v "${Q%% *}" >/dev/null 2>&1 || { echo "SKIP $t: no ${Q%% *}"; continue; }
    d=$out/$t; mkdir -p "$d"
    sh tools/build-libc.sh "$t" "$d" 2> "$d/build.err" &&
    sh tools/build-rt.sh "$t" "$d" 2>> "$d/build.err" || {
        echo "$t: the library does not build:"; head -3 "$d/build.err"
        fail=1; continue; }
    for f in boot io; do
        "$EMBCC" --target="$t" -c "$H/$f.c" -o "$d/$f.o" || exit 1
    done
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target="$t" $opt -Ilib/libc/include -c "$prog" \
            -o "$d/p$opt.o" || { echo "$t $opt: does not compile"; fail=1
                                 continue; }
        case $t in
            riscv*)    hv=EMBCC_RISCV_HARNESS ;;
            thumbv8m*) hv=EMBCC_M33_HARNESS ;;
            *)         hv=EMBCC_THUMB_HARNESS ;;
        esac
        env "$hv=$d" sh "$H/link.sh" "$d/p$opt.elf" "$d/p$opt.o" \
            "$d/libc.a" "$d/librt.a" > "$d/ld.txt" 2>&1 || {
            echo "$t $opt: does not link:"; head -3 "$d/ld.txt"; fail=1
            continue; }
        # shellcheck disable=SC2086
        sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-60}" --until '==END==' \
            $Q -kernel "$d/p$opt.elf" > "$d/run$opt.raw" 2>/dev/null
        sed -n '1,/==END==/p' "$d/run$opt.raw" > "$d/run$opt.txt"
        if ! diff -u "$out/ref.txt" "$d/run$opt.txt" > "$d/run$opt.diff"; then
            echo "$t $opt: libc does not agree with x86-64:"
            head -12 "$d/run$opt.diff"; fail=1
        fi
    done
    [ "$fail" = 0 ] && echo "$t: libc agrees with x86-64 at -O0, -O2 and -Os"
done
[ "$fail" = 0 ] || exit 1
echo "lib/libc runs on the boards and prints what it prints on x86-64"
