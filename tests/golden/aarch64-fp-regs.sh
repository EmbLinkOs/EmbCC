#!/bin/sh
# AArch64: a double where the code needs it, not in memory on the way.
#
# fdlibm takes doubles apart and puts them back (tests/exec/fp-bits.c has
# the values; this is the shape):
#
#   - a double parameter only floating-point ops read stays in d0: an
#     `and #imm` elsewhere in the function used to take vreg 0 out of the
#     float class (cg_float_vregs read every op's unused `c` field, 0);
#   - a value both kinds of op touch crosses the register files with one
#     fmov, never a store and a load -- and lives in the file most of its
#     uses are in: fdlibm's x, read by many float ops and by the one shift
#     that takes its high word, stays in a d register;
#   - a double constant is one `ldr dN, <literal>` from the function's
#     pool, not mov/movk/movk/movk/fmov; +0.0 is `fmov d, xzr`.
set -u
echo "TEST-MARKER aarch64-fp-regs"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
out=tests/golden/out/aarch64-fp-regs
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/f.c" <<'EOF'
typedef union { double value; struct { unsigned lsw, msw; } parts; } shape;
double mix(double x)
{
    double y = x * 3.0;
    unsigned hi;
    { shape u; u.value = y; hi = u.parts.msw; }
    return y + (double)(hi & 1);
}
double my_fabs(double x)
{
    unsigned hi;
    { shape u; u.value = x; hi = u.parts.msw; }
    { shape u; u.value = x; u.parts.msw = hi & 0x7fffffffu; x = u.value; }
    return x;
}
double poly(double z)
{
    return 1.66666666666666019037e-01 + z * (-2.77777777770155933842e-03 +
           z * (6.61375632143793436117e-05 + z * (-1.65339022054652515390e-06 +
           z * 4.13813679705723846039e-08)));
}
double zero(void) { return 0.0; }
double kt(double x)
{
    int hx;
    { shape u; u.value = x; hx = (int)u.parts.msw; }
    if (hx > 0x3fe00000)
        return x * x * x + x * 0.5 + x;
    return x * x - x;
}
EOF
"$EMBCC" --target=aarch64-elf -O2 -c "$out/f.c" -o "$out/f.o" || fail "f.c"
"$OBJDUMP" -d --no-show-raw-insn "$out/f.o" > "$out/f.dis"
fn() { sed -n "/<$1>:/,/^\$/p" "$out/f.dis"; }

fn mix > "$out/mix.dis"
grep -q 'fmul	d[0-9]*, d0, ' "$out/mix.dis" ||
    { cat "$out/mix.dis"; fail "mix: x is not read from d0 (out of the float class?)"; }
if grep -q 'sp' "$out/mix.dis"; then
    cat "$out/mix.dis"; fail "mix: a frame or a stack access; every value has a register"
fi
grep -q 'fmov	x[0-9]*, d[0-9]*' "$out/mix.dis" ||
    { cat "$out/mix.dis"; fail "mix: y's bits do not cross with fmov"; }

fn my_fabs > "$out/fabs.dis"
if grep -Eq 'str	d0|ldr	x[0-9]+, \[sp' "$out/fabs.dis"; then
    cat "$out/fabs.dis"; fail "my_fabs: the parameter goes through memory to an x register"
fi
grep -q 'fmov	x[0-9]*, d0' "$out/fabs.dis" ||
    { cat "$out/fabs.dis"; fail "my_fabs: no fmov from d0"; }

fn poly > "$out/poly.dis"
n=$(grep -c 'ldr	d[0-9]*, 0x' "$out/poly.dis")
[ "$n" = 5 ] || { cat "$out/poly.dis"; fail "poly: $n literal loads, want one per constant (5)"; }
if grep -q 'movk' "$out/poly.dis"; then
    cat "$out/poly.dis"; fail "poly: a constant built with movk"
fi
fn zero | grep -q 'fmov	d0, xzr' || { fn zero; fail "zero: 0.0 is not fmov from xzr"; }
fn kt > "$out/kt.dis"
if grep -q 'fmov	d[0-9]*, x' "$out/kt.dis"; then
    cat "$out/kt.dis"; fail "kt: x lives in an x register and every float use moves it back"
fi
grep -q 'fmov	x[0-9]*, d' "$out/kt.dis" ||
    { cat "$out/kt.dis"; fail "kt: no fmov out for the high word"; }
echo "AArch64: a float parameter in d0, crossings by fmov, mixed values in the file most uses are in,"
echo "and constants from the literal pool"
