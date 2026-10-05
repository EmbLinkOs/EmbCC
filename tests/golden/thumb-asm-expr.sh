#!/bin/sh
# Cortex-M asm: an immediate may be a constant expression, as in GNU as.
#
# FreeRTOS's ARM_CM4F and ARM_CM7 ports enable the FPU with
# `orr r1, r1, #( 0xf << 20 )`, and the assembler took `#(` for the end of
# the instruction and refused it. An immediate now runs to the next comma,
# spaces included, and is C's integer operators over C's literals with
# parentheses -- in data-processing operands, movw, cmp, and a memory
# operand's offset. Each instruction below is checked against what
# llvm-mc assembles from the same text (`mov` with a value movw holds is
# movw here, as everywhere in this assembler; the bytes differ, the
# value does not). A name in an immediate is still refused.
set -u
echo "TEST-MARKER thumb-asm-expr"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
command -v "$MC" >/dev/null 2>&1 || { echo "SKIP: no $MC"; exit 0; }
out=tests/golden/out/thumb-asm-expr
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/ins.s" <<'EOF'
orr r1, r1, #( 0xf << 20 )
bic r0, r0, #~0xffffff00
cmp r0, #(255 & 0x7f)
movw r0, #(0x1234 + 0x10)
add r3, r3, #(16*4)
sub r3, r3, #(1 << 2) - 1
ldr r0, [r1, #(4 * 3)]
str r0, [r1, #-(2 * 4)]
and r0, r0, #0x3f0 >> 4
eor r2, r2, #(1u << 31) | (1 << 30)
tst r1, #(0x80 % 0x30) ^ 0x8
mov r2, #(1<<4)|3
EOF
{
    printf '%s\n' '__attribute__((naked)) void ex(void) { __asm volatile('
    sed 's/.*/    "&\\n"/' "$out/ins.s"
    printf '%s\n' '    "bx lr\n"); }'
} > "$out/ex.c"
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/ex.c" -o "$out/ex.o" ||
    fail "an expression immediate was refused"
"$OBJDUMP" -d "$out/ex.o" | sed -n '/<ex>:/,$p' |
    awk -F'\t' 'NF >= 3 { sub(/^ *[0-9a-f]+: */, "", $1); gsub(/ /, "", $1);
                           print $1 }' > "$out/got.hex"
"$MC" -triple=thumbv7m -show-encoding "$out/ins.s" 2>&1 |
    sed -n 's/.*encoding: \[\(.*\)\]/\1/p' |
    awk -F, '{ s = ""; for (i = 1; i <= NF; i += 2) { a = $(i); b = $(i + 1);
               gsub(/0x| /, "", a); gsub(/0x| /, "", b); s = s b a }
               print s }' > "$out/want.hex"
# the mov: llvm-mc's mov.w against this assembler's movw of the same value
sed '$d' "$out/want.hex" > "$out/want-cut.hex"
sed -n '1,11p' "$out/got.hex" > "$out/got-cut.hex"
[ "$(wc -l < "$out/want-cut.hex")" -eq 11 ] || { cat "$out/want.hex"; fail "llvm-mc did not assemble the reference"; }
diff "$out/want-cut.hex" "$out/got-cut.hex" ||
    fail "an expression immediate assembled differently from llvm-mc"
sed -n '12p' "$out/got.hex" | grep -qi '^f2400213$' ||
    { sed -n '12p' "$out/got.hex"; fail "mov r2, #(1<<4)|3 is not movw r2, #19"; }
cat > "$out/bad.c" <<'EOF'
__attribute__((naked)) void bad(void) { __asm volatile("orr r1, r1, #(FOO << 2)\n"); }
EOF
if "$EMBCC" --target=thumbv7m-none-eabi -c "$out/bad.c" -o "$out/bad.o" 2>/dev/null; then
    fail "a name in an immediate was accepted"
fi
echo "thumb-asm-expr: 12 expression immediates assemble as llvm-mc assembles them"
