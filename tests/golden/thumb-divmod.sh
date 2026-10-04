#!/bin/sh
# A quotient and a remainder by one constant share their divide on Thumb.
#
# ARMv7-M has udiv and sdiv in hardware, and nothing here turns a divide
# by a constant into a multiply (pass_divmagic needs 64-bit registers), so
# `v % 10; v / 10` -- every integer-to-text loop -- was two divides, each
# two to twelve cycles on a Cortex-M4. Paired, the quotient is computed
# once and the remainder is `v - q*10`: one mls, because a multiply whose
# only reader is the add or subtract right after it keeps its constant in
# a register (mla_keeps_reg) rather than becoming shifted adds.
set -u
echo "TEST-MARKER thumb-divmod"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-divmod
rm -rf "$out"; mkdir -p "$out"

cat > "$out/d.c" <<'EOF'
int digits(char *b, unsigned v)
{
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    return n;
}
int spair(int a, int *r) { int q = a / -7; *r = a % -7; return q; }
unsigned rem_only(unsigned a) { return a % 10; }
int macc(const int *x, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += x[i] * 10;
    return s;
}
EOF
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/d.c" -o "$out/d.o" 2> "$out/cc.log" || {
    echo "FAIL: could not compile:"; cat "$out/cc.log"; exit 1; }
"$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/d.o" > "$out/d.dis"
count() {                       # count FUNC REGEX
    awk -v f="<$1>:" -v re="$2" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                                  on && $0 ~ re { n++ } END { print n + 0 }' "$out/d.dis"
}
[ "$(count digits '\tudiv\t')" = 1 ] && [ "$(count digits '\tmls\t')" = 1 ] || {
    echo "FAIL: the digit loop should divide once and take the remainder with"
    echo "      one mls:"; cat "$out/d.dis"; exit 1; }
echo "v % 10 and v / 10: one udiv and one mls"
[ "$(count spair '\tsdiv\t')" = 1 ] && [ "$(count spair '\tmls\t')" = 1 ] || {
    echo "FAIL: a signed pair by -7 should be one sdiv and one mls:"; cat "$out/d.dis"; exit 1; }
echo "a / -7 and a % -7: one sdiv and one mls"
[ "$(count rem_only '\tudiv\t')" = 1 ] && [ "$(count rem_only '\tmls\t')" = 1 ] || {
    echo "FAIL: a remainder alone should stay udiv and mls:"; cat "$out/d.dis"; exit 1; }
echo "a remainder alone is still udiv and mls"
[ "$(count macc '\tmla\t')" -ge 1 ] && [ "$(count macc 'lsl #')" = 0 ] || {
    echo "FAIL: s += x[i] * 10 should be an mla with 10 in a register, not"
    echo "      shifted adds:"; cat "$out/d.dis"; exit 1; }
echo "s += x * 10: mla with the 10 in a register, no shifted adds"
