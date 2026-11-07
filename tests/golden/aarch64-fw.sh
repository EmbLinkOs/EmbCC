#!/bin/sh
# AArch64 firmware, linked by the driver: `embcc --target=aarch64-elf
# start.S fw.c --link-script=fw.ld -o fw.elf` runs embld in-process (it
# once refused: "embld does not read AArch64 objects"). The startup's
# `ldr x0, =__stack_top` is a literal pool and its `#(3 << 20)` an
# expression, both of which the assembler now takes; the program prints
# through semihosting and exits 42 on QEMU's virt board.
set -u
echo "TEST-MARKER aarch64-fw"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/aarch64-fw
src=tests/golden/aarch64-fw
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
"$EMBCC" --target=aarch64-elf -O2 -nostdlib --link-script=$src/fw.ld \
    $src/start.S $src/fw.c -o "$out/fw.elf" || { echo "FAIL: the driver does not link it"; exit 1; }
command -v llvm-readelf >/dev/null 2>&1 &&
    { llvm-readelf -hW "$out/fw.elf" | grep -q 'AArch64' || { echo "FAIL: not an AArch64 image"; exit 1; }; }
command -v "${EMBCC_QEMU_AARCH64:-qemu-system-aarch64}" >/dev/null 2>&1 ||
    { echo "aarch64-fw: linked (no qemu-system-aarch64 to run it)"; exit 0; }
got=$(sh tests/harness/aarch64/run.sh "$out/fw.elf" 2>&1); rc=$?   # (WRITE0 is QEMU's stderr)
[ "$rc" = 42 ] || { echo "FAIL: exit $rc, not 42"; exit 1; }
[ "$got" = "fw ok" ] || { echo "FAIL: printed '$got', not 'fw ok'"; exit 1; }
echo "aarch64-fw: ok"
