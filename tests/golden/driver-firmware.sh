#!/bin/sh
# The driver links firmware: `embcc ... a.o b.o -T fw.ld -o fw.elf`, the
# link line of every embedded Makefile (and CMake's), with no source on
# it, or with one source, an assembly start among the objects.
#
# Each image is RUN, on QEMU's Cortex-M3 and RISC-V virt boards, using the
# firmware tests/golden/ldscript.sh links with embld directly; the point
# here is that the driver hands embld the same link. Then gcc's link
# options -- -Wl,-T, -nostdlib, -lgcc, -L/-l with a real archive -- and
# the refusals: no memory map (for objects, and for two sources, which
# compile and then need it too), a library not found.
set -u
echo "TEST-MARKER driver-firmware"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBAR=${EMBAR:-./embar}
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
d=tests/golden/ldscript
out=tests/golden/out/driver-firmware
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

T=--target=thumbv7em-none-eabi
for f in startup prog; do
    "$EMBCC" $T -O2 -c "$d/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBCC" $T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" || fail "io.c"
printf 'hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n' > "$out/want.txt"
run_arm() {
    command -v "$QARM" >/dev/null 2>&1 || return 0
    EMBCC_QEMU_ARM=$QARM sh tests/harness/thumb/run.sh "$1" > "$1.txt" 2>&1
    cmp -s "$1.txt" "$out/want.txt" || { cat "$1.txt"; fail "$1 did not run as linked"; }
}

# 1. objects only: the link step of a build
"$EMBCC" $T -T "$d/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/a.elf" || fail "the driver did not link the objects"
run_arm "$out/a.elf"
echo "objects only: linked with -T and run"

# 2. one source among the objects, and the script through -Wl
"$EMBCC" $T -O2 -Wl,-T,"$d/stm32.ld" "$d/prog.c" "$out/startup.o" \
    "$out/io.o" -o "$out/b.elf" || fail "a source among the objects"
run_arm "$out/b.elf"
echo "a source and objects, -Wl,-T: linked and run"

# 3. -nostdlib with -lgcc, and a library of our own through -L/-l
mkdir -p "$out/lib"
"$EMBAR" rcs "$out/lib/libuart.a" "$out/io.o" || fail "embar"
"$EMBCC" $T -T "$d/stm32.ld" -nostdlib -lgcc "$out/startup.o" "$out/prog.o" \
    -L "$out/lib" -luart -o "$out/c.elf" || fail "-nostdlib -lgcc -L -l"
run_arm "$out/c.elf"
echo "-nostdlib, -lgcc and -L/-luart: linked and run"

# 4. RISC-V: an assembly start, linked without -c
for X in 32 64; do
    R=--target=riscv$X-unknown-elf
    for f in reset rvprog; do
        "$EMBCC" $R -O2 -c "$d/$f.c" -o "$out/$f$X.o" || fail "$f.c rv$X"
    done
    "$EMBCC" $R -O2 -c tests/harness/riscv/io.c -o "$out/io$X.o" || fail "io rv$X"
    "$EMBCC" $R -T "$d/sifive.ld" "$d/start.S" "$out/reset$X.o" \
        "$out/rvprog$X.o" "$out/io$X.o" -o "$out/rv$X.elf" ||
        fail "rv$X: start.S and objects did not link"
    QR=${EMBCC_QEMU_RISCV:-qemu-system-riscv$X}
    if command -v "$QR" >/dev/null 2>&1; then
        sh tests/harness/riscv/run.sh "$out/rv$X.elf" $X > "$out/rv$X.txt" 2>&1
        printf 'riscv data\n19088743 52719 0 100 \n' > "$out/rwant.txt"
        cmp -s "$out/rv$X.txt" "$out/rwant.txt" ||
            { cat "$out/rv$X.txt"; fail "rv$X image did not run"; }
    fi
done
echo "RISC-V: start.S assembled and linked with the objects, run at RV32 and RV64"

# 5. refusals
refuse() {      # refuse TAG PATTERN ARGS...
    tag=$1; pat=$2; shift 2
    if "$EMBCC" "$@" > "$out/$tag.txt" 2>&1; then fail "$tag should be refused"; fi
    grep -q "$pat" "$out/$tag.txt" || { cat "$out/$tag.txt"; fail "$tag: want '$pat'"; }
}
refuse nomap "needs its memory map" $T "$out/startup.o" "$out/prog.o" -o "$out/x.elf"
refuse nolib "cannot find libnosys.a" $T -T "$d/stm32.ld" "$out/prog.o" -lnosys -o "$out/x.elf"
# two sources compile (driver-multi.sh), and their link needs the map too
refuse twosrc "needs its memory map" $T "$d/prog.c" "$d/startup.c" -o "$out/x.elf"
ls "$out" | grep -q 'embcc-tmp' && fail "twosrc: temporary objects left behind"
echo "refused: no memory map (from objects, and from two sources), a missing library"
