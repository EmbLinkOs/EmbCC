#!/bin/sh
# Objects built by clang, linked against EmbCC's runtime.
#
# clang and GCC compile a 64-bit divide, a block copy or clear, and every
# soft-float operation into calls to the ARM run-time ABI's helpers --
# __aeabi_uldivmod, __aeabi_memclr4, __aeabi_dadd ... -- and so does every
# library built with them: CMSIS-DSP, a vendor's HAL, anything compiled by
# arm-none-eabi-gcc. lib/rt had none of them for ARMv7-M, so none of that
# linked against EmbCC's runtime. lib/rt/aeabi.c now has the set.
#
# embedded-aeabi.c, compiled by clang for ARMv7E-M (soft and hard float),
# ARMv7-M and ARMv6-M, is linked by embld with EmbCC's lib/rt, lib/libc
# and harness, and run on the Cortex-M4 board (which runs all four); its
# output must be the host's. The test first checks that clang's object
# does call the helpers, so a pass is not vacuous.
set -u
echo "TEST-MARKER thumb-aeabi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU not found"; exit 0; }
command -v "$CLANG" >/dev/null 2>&1 || { echo "SKIP: no clang"; exit 0; }
NM=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$NM" >/dev/null 2>&1 || { echo "SKIP: llvm-nm not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-aeabi
rm -rf "$out"; mkdir -p "$out"
EMBCC=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLD=${EMBLD:-$PWD/embld}
export EMBCC EMBLD
CI=$("$CLANG" -print-resource-dir)/include

cc -std=c11 -w -o "$out/host" tests/golden/embedded-aeabi.c \
   tests/harness/thumb/hostio.c || { echo "FAIL: host build"; exit 1; }
"$out/host" > "$out/ref.txt" || { echo "FAIL: host run"; exit 1; }

fail=0
for cfg in "thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft" \
           "thumbv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16" \
           "thumbv7m-none-eabi -mcpu=cortex-m3" \
           "thumbv6m-none-eabi -mcpu=cortex-m0"; do
    set -- $cfg; T=$1; shift; X="$*"
    L=$out/$T; mkdir -p "$L"
    { sh tools/build-rt.sh $T "$L" && sh tools/build-libc.sh $T "$L"; } \
        > "$L/build.log" 2>&1 || { echo "FAIL: $T: the runtime does not build"; fail=1; continue; }
    for f in boot io; do
        "$EMBCC" --target=$T -DSRAM_TOP=0x20400000u \
            -c tests/harness/thumb-m4f/$f.c -o "$L/$f.o" || { fail=1; continue 2; }
    done
    for opt in -O1 -O2; do
        o=$L/p$opt
        "$CLANG" --target=$T $X $opt -ffreestanding -nostdinc -isystem "$CI" -w \
            -c tests/golden/embedded-aeabi.c -o $o.o || {
            echo "FAIL: $T $opt: clang does not compile it"; fail=1; continue; }
        n=$("$NM" -u $o.o | grep -c '__aeabi_' || true)
        [ "$n" -ge 4 ] || { echo "FAIL: $T $opt: clang called only $n helpers"; fail=1; continue; }
        EMBCC_THUMB_HARNESS=$L sh tests/harness/thumb-m4f/link.sh $o.elf $o.o \
            "$L/libc.a" "$L/librt.a" > $o.lerr 2>&1 || {
            echo "FAIL: $T $opt: does not link: $(head -1 $o.lerr)"; fail=1; continue; }
        sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' \
            "$QEMU" -M mps2-an386 -cpu cortex-m4 -nographic -kernel $o.elf \
            > $o.txt 2>/dev/null
        if diff -u "$out/ref.txt" $o.txt > $o.diff; then
            echo "$T $opt: clang's object calls $n helpers and agrees with the host"
        else
            echo "FAIL: $T $opt: disagrees with the host:"; head -12 $o.diff; fail=1
        fi
    done
done
exit $fail
