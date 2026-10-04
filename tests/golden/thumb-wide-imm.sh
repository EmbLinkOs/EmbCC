#!/bin/sh
# 64-bit operations with a constant on Thumb, half by half.
#
# A `long long` (and a soft-float double's bits) lives in a register pair,
# and an AND with 0x000fffffffffffff is two separate questions: the low
# word with all ones, which is a copy, and the high word with 0xfffff,
# which is a ubfx. Building both words of the constant in r9/r10 and
# ANDing each took four instructions and two pushed registers. Likewise a
# shift by a constant goes straight from the operand's pair into the
# result's, the bits crossing between words as an orr's shifted operand;
# `(int)(x >> 52) & 0x7ff`, a double's exponent, is one ubfx of the high
# word; and `if (x >> 63)` is `cmp hi, #0`.
set -u
echo "TEST-MARKER thumb-wide-imm"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-wide-imm
rm -rf "$out"; mkdir -p "$out"

cat > "$out/w.c" <<'EOF'
typedef unsigned long long u64;
u64 mant(u64 x) { return x & 0x000fffffffffffffULL; }
u64 hibit(u64 x) { return x | 0x8000000000000000ULL; }
int expo(u64 x) { return (int)(x >> 52) & 0x7ff; }
u64 shr20(u64 x) { return x >> 20; }
int neg(u64 x) { if (x >> 63) return 1; return 0; }
EOF
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/w.c" -o "$out/w.o" 2> "$out/cc.log" || {
    echo "FAIL: could not compile:"; cat "$out/cc.log"; exit 1; }
"$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/w.o" > "$out/w.dis"
body() {                        # body FUNC -> the function's instructions
    awk -v f="<$1>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                      on && /^ *[0-9a-f]+:/' "$out/w.dis"
}
n() { body "$1" | grep -cE "$2" || true; }
total() { body "$1" | wc -l | tr -d ' '; }
fail() { echo "FAIL: $1:"; body "$2"; exit 1; }

[ "$(n mant 'ubfx')" = 1 ] && [ "$(n mant 'movw|movt')" = 0 ] ||
    fail "mant should be one ubfx on the high word, no constant built" mant
echo "x & 0x000fffffffffffff: a ubfx on the high word, nothing built"
[ "$(n hibit 'orr')" = 1 ] && [ "$(n hibit 'movw|movt|mov.w')" = 0 ] ||
    fail "hibit should be one orr of #0x80000000 on the high word" hibit
echo "x | 1 << 63: one orr on the high word"
[ "$(n expo 'ubfx')" = 1 ] && [ "$(total expo)" -le 3 ] ||
    fail "expo should be one ubfx of the high word" expo
echo "(int)(x >> 52) & 0x7ff: one ubfx of the high word"
[ "$(n shr20 'orr')" = 1 ] && [ "$(total shr20)" -le 4 ] ||
    fail "shr20 should be lsr, orr with a shifted operand, lsr" shr20
echo "x >> 20: three instructions, the crossing bits an orr's shifted operand"
[ "$(n neg 'cmp')" = 1 ] && [ "$(n neg 'lsr|orrs')" = 0 ] ||
    fail "neg should test the high word's sign with one cmp" neg
echo "if (x >> 63): cmp of the high word"
