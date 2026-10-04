#!/bin/sh
# An RTOS's context switch, written in assembly, assembled by EmbCC and
# RUN: two tasks on a Cortex-M4F taking turns through PendSV.
#
# tests/golden/rtos/switch.S is the shape every Cortex-M RTOS port has.
# PendSV saves the outgoing task's r4-r11 and EXC_RETURN on its own stack
# -- and s16-s31, under an IT block, when the task used the FPU -- calls C
# for the next task, and restores that one's. SVC starts the first. It
# needs everything a hand-written port does: `#` immediates, the special
# registers, ldm/stm with writeback and register ranges, vldm/vstm, an IT
# block, a literal load, `ldr rd, =sym`, movw/movt of a symbol, `b` and
# `bl` to C, `.word` of a C variable, `.thumb_func`, `.align`. Before,
# EmbCC's assembler cut `tst lr, #0x10` at the `#` as a comment, took
# `psp` and `eq` for undefined symbols, had no ldm/stm/svc/IT, and read
# `.align 2` as two bytes.
#
# main.c sets VTOR to its own vector table, builds two task stacks and
# starts them. Each task leaves its own value in s20 -- saved only by the
# vstmdb/vldmia path -- and checks it after every switch. A handler
# without that path reports `lost 5`.
set -u
echo "TEST-MARKER thumb-rtos"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}

out=tests/golden/out/thumb-rtos
rm -rf "$out"; mkdir -p "$out"
T=thumbv7em-none-eabi
H=tests/harness/thumb-m4f
D=tests/golden/rtos
fail=0

# ---- .align is a power of two, and code is padded with nops --------------
# GNU as reads `.align n` as 2^n on ARM and RISC-V; EmbCC read n bytes.
MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1; then
    printf '\t.syntax unified\n\t.thumb\n\tnop\n\t.align 4\n\tnop\n\t.balign 8\n\tnop\n\t.p2align 3\n\tnop\n' > "$out/al.s"
    printf '\t.half 1\n\t.align 3\n\taddi a0, a1, 1\n\t.align 2\n\t.half 1\n\t.align 3\n\t.half 1\n' > "$out/rv.s"
    for spec in "$T thumbv7em al" "riscv32-unknown-elf riscv32 rv"; do
        set -- $spec
        "$EMBCC" --target=$1 -c "$out/$3.s" -o "$out/$3.o" || {
            echo "FAIL: $3.s does not assemble for $1"; fail=1; continue; }
        "$MC" -triple=$2 $( [ $3 = rv ] && echo -mattr=+c ) -filetype=obj \
            "$out/$3.s" -o "$out/$3-ref.o" || { echo "FAIL: llvm-mc on $3.s"; fail=1; continue; }
        "$OBJCOPY" -O binary --only-section=.text "$out/$3.o" "$out/$3.bin"
        "$OBJCOPY" -O binary --only-section=.text "$out/$3-ref.o" "$out/$3-ref.bin"
        cmp -s "$out/$3.bin" "$out/$3-ref.bin" || {
            echo "FAIL: .align on $1 does not pad as llvm-mc does:"
            od -An -tx1 "$out/$3.bin"; od -An -tx1 "$out/$3-ref.bin"; fail=1; }
    done
    [ $fail = 0 ] && echo ".align is 2^n and code pads with nops, as llvm-mc does, on Thumb and RISC-V"
fi

command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the run: $QEMU absent"; exit $fail; }

EMBCC_THUMB_HARNESS=$PWD/$out; export EMBCC_THUMB_HARNESS
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
"$EMBCC" --target=$T -c "$D/switch.S" -o "$out/switch.o" || {
    echo "FAIL: the context switch does not assemble"; exit 1; }
want='A0 B0 A1 B1 A2 B2 
lost 0 
==END=='
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$D/main.c" -o "$out/main.o" || {
        echo "FAIL $opt: main.c does not compile"; fail=1; continue; }
    sh "$H/link.sh" "$out/rtos.elf" "$out/main.o" "$out/switch.o" \
        > "$out/ln.txt" 2>&1 || {
        echo "FAIL $opt: does not link"; head -3 "$out/ln.txt"; fail=1; continue; }
    got=$(sh "$H/run.sh" "$out/rtos.elf" | sed -n '/^A0/,/==END==/p')
    [ "$got" = "$want" ] || {
        echo "FAIL $opt: the tasks did not take turns intact:"
        printf '%s\n' "$got" | sed 's/^/  | /'; fail=1; }
done
[ $fail = 0 ] || exit 1
echo "a PendSV context switch assembled by EmbCC runs two tasks on a
Cortex-M4F at -O0, -O1, -O2 and -Os, the FPU context included"
