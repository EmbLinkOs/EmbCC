#!/bin/sh
# 64-bit operations with a constant at RV32, half by half (logic_half,
# shift64_imm_to), and the tests a soft-float routine makes of a double's
# bits: `if (x >> 63)` is the high word's sign, and `(m >> 52) & 1 == 0`
# one andi of the high word shifted. Each was a pair built in t2/t3, moves
# through t0/t1, an or of both halves and a seqz before the branch.
set -u
echo "TEST-MARKER riscv-wide-imm"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/riscv-wide-imm
rm -rf "$out"; mkdir -p "$out"

cat > "$out/w.c" <<'EOF'
typedef unsigned long long u64;
u64 mant(u64 x) { return x & 0x000fffffffffffffULL; }
u64 quiet(u64 x) { return x | 0x0008000000000000ULL; }
u64 shr20(u64 x) { return x >> 20; }
int neg(u64 x) { if (x >> 63) return 1; return 0; }
int bit52(u64 m) { return ((m >> 52) & 1) == 0; }
EOF
"$EMBCC" --target=riscv32-unknown-elf -O2 -c "$out/w.c" -o "$out/w.o" 2> "$out/cc.log" || {
    echo "FAIL: could not compile:"; cat "$out/cc.log"; exit 1; }
"$OD" -d --no-show-raw-insn "$out/w.o" > "$out/w.dis"
body() {
    awk -v f="<$1>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                      on && /^ *[0-9a-f]+:/' "$out/w.dis"
}
n() { body "$1" | grep -cE "$2" || true; }
total() { body "$1" | wc -l | tr -d ' '; }
fail() { echo "FAIL: $1:"; body "$2"; exit 1; }

[ "$(n mant '\t(lui|li)\t')" = 0 ] && [ "$(total mant)" -le 3 ] ||
    fail "mant should keep the low word and shift the high word left and back" mant
echo "x & 0x000fffffffffffff: the high word shifted left and back, nothing built"
[ "$(n quiet '\tlui\t')" = 1 ] && [ "$(n quiet '\tor\t')" = 1 ] && [ "$(total quiet)" -le 3 ] ||
    fail "quiet should be one lui and one or on the high word" quiet
echo "x | 1 << 51: one lui, one or"
[ "$(total shr20)" -le 5 ] && [ "$(n shr20 '\tmv\t')" = 0 ] ||
    fail "shr20 should be four shifts and an or, no moves" shr20
echo "x >> 20: no moves through t0/t1"
[ "$(n neg 'bltz|bgez|slti|srli')" -ge 1 ] && [ "$(n neg '\tor\t|seqz')" = 0 ] ||
    fail "neg should read the high word's sign" neg
echo "if (x >> 63): the high word's sign"
[ "$(n bit52 'andi')" -le 1 ] && [ "$(n bit52 '\tor\t')" = 0 ] ||
    fail "bit52 should be a shift of the high word and one test" bit52
echo "(m >> 52 & 1) == 0: one word, no or of the halves"
