#!/bin/sh
# Single-precision arithmetic on the Cortex-M4F's FPU, RUN on a Cortex-M4F.
#
# FPv4-SP-D16 computes single precision only, so this is `float` and
# nothing else -- a `double` still goes to __adddf3 on the same part, and
# that is the silicon's limit rather than a shortcut.
#
# It is run on QEMU's mps2-an386, which is a Cortex-M4 WITH the FPU. The
# existing ARMv7-M harness is an lm3s6965evb, a Cortex-M3 with none, so
# VFP code there faults -- which is why tests/harness/thumb-m4f exists and
# why this test could not simply reuse the other one. Two things differ
# and both belong to the part, not the compiler: the FPU is OFF at reset
# and CPACR has to enable both CP10 and CP11, and the board's UART is the
# CMSDK one whose transmitter must be enabled before it prints.
#
# The answers are compared as BIT PATTERNS against the host, because a
# float that is close is still wrong and "%f" would hide it.
set -u
echo "TEST-MARKER thumb-fpu"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
"$QEMU" -machine help 2>/dev/null | grep -q mps2-an386 || {
    echo "SKIP: this QEMU has no mps2-an386 (a Cortex-M4 with an FPU)"
    exit 0; }

T=thumbv7em-none-eabi
H=$EMBCC_ROOT/tests/harness/thumb-m4f
out=tests/golden/out/thumb-fpu
rm -rf "$out"; mkdir -p "$out"
EMBCC_THUMB_HARNESS=$PWD/$out; export EMBCC_THUMB_HARNESS

for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the M4F harness does not compile"; exit 1; }
done
"$EMBCC" --target=$T -Os -c lib/rt/softfp.c -o "$out/softfp.o" || {
    echo "the soft-float runtime does not compile"; exit 1; }

cat > "$out/fp.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);
union fu { float f; unsigned u; };
static unsigned B(float f) { union fu x; x.f = f; return x.u; }
static float   F(unsigned u) { union fu x; x.u = u; return x.f; }
static void h8(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
/* volatile so nothing folds at compile time: the FPU has to be the thing
 * that computes these. */
volatile float a = 3.5f, b = 0.25f, c = -2.0f, z = 0.0f;
int main(void)
{
    h8(B(a + b));
    h8(B(a - b));
    h8(B(a * b));
    h8(B(a / b));
    h8(B(-a));
    h8(B(c * c));
    h8(B(F(0x40800000u) * F(0x40000000u)));
    /* The corners worth having: a signed zero must keep its sign through
     * a multiply, and dividing by zero must give an infinity rather than
     * a trap, because FPSCR's exception enables are off at reset and
     * this harness does not turn them on. */
    h8(B(-z));
    h8(B(z * c));
    h8(B(a / z));
    h8(B(-a / z));
    puts_("\n==END==\n");
    return 0;
}
CEOF

cc -w -o "$out/host" "$out/fp.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
[ -n "$want" ] || { echo "the host reference printed nothing"; exit 1; }

# Both ways must agree, at four levels: the FPU and the soft-float
# runtime are two implementations of the same arithmetic, and a
# disagreement means one of them is wrong.
for fpu in 0 1; do
    for opt in -O0 -O1 -O2 -Os; do
        EMBCC_T_FPU=$fpu "$EMBCC" --target=$T $opt -c "$out/fp.c" \
            -o "$out/fp.o" || { echo "FPU=$fpu $opt: does not compile"
                                exit 1; }
        sh "$H/link.sh" "$out/fp.elf" "$out/fp.o" "$out/softfp.o" \
            > "$out/ln.log" 2>&1 || {
            echo "FPU=$fpu $opt: does not link"; head -3 "$out/ln.log"
            exit 1; }
        got=$(sh "$H/run.sh" "$out/fp.elf" 2>&1 | head -1)
        [ "$got" = "$want" ] || {
            echo "FPU=$fpu $opt: disagrees with the host"
            echo "  want: $want"
            echo "  got:  $got"; exit 1; }
    done
done
echo "single-precision arithmetic is bit-identical to the host on a
Cortex-M4F, with the FPU and with the soft-float runtime, at four levels"

# The FPU path must actually USE the FPU -- agreeing with the host while
# quietly calling __addsf3 would pass the check above and mean nothing.
EMBCC_T_FPU=1 "$EMBCC" --target=$T -Os -c "$out/fp.c" -o "$out/hard.o"
EMBCC_T_FPU=0 "$EMBCC" --target=$T -Os -c "$out/fp.c" -o "$out/soft.o"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
if command -v "$OD" >/dev/null 2>&1; then
    nh=$("$OD" -d --triple=thumbv7em --mattr=+vfp4 "$out/hard.o" |
         grep -cE 'v(add|sub|mul|div|neg)\.f32' || true)
    [ "$nh" -ge 5 ] || {
        echo "the FPU build has only $nh VFP arithmetic instructions --
it is still calling the soft-float runtime"; exit 1; }
    ns=$("$OD" -d --triple=thumbv7em --mattr=+vfp4 "$out/soft.o" |
         grep -cE 'v(add|sub|mul|div)\.f32' || true)
    [ "$ns" = 0 ] || {
        echo "the soft-float build emitted $ns VFP instructions, and the
FPU was not asked for"; exit 1; }
    echo "the FPU build uses $nh VFP instructions and the soft one none"
else
    echo "SKIP the disassembly check: no objdump"
fi

# And a double still goes to the runtime on this part, because FPv4-SP
# has no double arithmetic. Claiming otherwise would be the interesting
# way to be wrong.
printf 'double g(double x, double y){ return x * y + x; }\n' > "$out/d.c"
EMBCC_T_FPU=1 "$EMBCC" --target=$T -Os -c "$out/d.c" -o "$out/d.o" || {
    echo "the double file does not compile"; exit 1; }
if command -v "$OD" >/dev/null 2>&1; then
    "$OD" -d --triple=thumbv7em --mattr=+vfp4 "$out/d.o" |
        grep -qE 'v(add|mul)\.f64' && {
        echo "a double was computed on the FPU, and FPv4-SP-D16 has no
double arithmetic"; exit 1; }
    echo "and a double still goes to the soft-float runtime, as the part
requires"
fi

# ---- the FP register class, under pressure ----------------------------
# Twelve live floats, so the allocator has to place and reuse registers
# rather than keeping one value at a time. This is the case that would
# expose the dangerous shape: a value the FP allocator placed, written by
# the INTEGER path (which does not know about FP registers) and then read
# back from the register. cg_float_vregs prevents it -- a value stays in
# the class only if every instruction touching it is floating point, and a
# float LOAD is not -- so anything from memory keeps its slot. This test
# is what says that rule is actually holding.
cat > "$out/stress.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);
union fu { float f; unsigned u; };
static unsigned B(float f) { union fu x; x.f = f; return x.u; }
static void h8(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
volatile float t[12] = { 1.5f, 2.5f, 0.5f, 4.0f, 0.25f, 8.0f,
                         3.0f, 1.25f, 6.0f, 0.75f, 2.0f, 5.0f };
int main(void)
{
    float a = t[0], b = t[1], c = t[2], d = t[3], e = t[4], f = t[5];
    float g = t[6], h = t[7], i = t[8], j = t[9], k = t[10], l = t[11];
    float r1 = a * b + c * d, r2 = e * f + g * h, r3 = i * j + k * l;
    float r4 = r1 / r2, r5 = r2 / r3, r6 = r3 / r1;
    float s = r4 + r5 + r6 - a * b * c;
    h8(B(r1)); h8(B(r2)); h8(B(r3));
    h8(B(r4)); h8(B(r5)); h8(B(r6)); h8(B(s));
    puts_("\n==END==\n");
    return 0;
}
CEOF
cc -w -o "$out/shost" "$out/stress.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null || {
    echo "the stress host reference does not build"; exit 1; }
swant=$("$out/shost" | head -1)
for opt in -O0 -O1 -O2 -Os; do
    EMBCC_T_FPU=1 "$EMBCC" --target=$T $opt -c "$out/stress.c" \
        -o "$out/st.o" || { echo "stress $opt: does not compile"; exit 1; }
    sh "$H/link.sh" "$out/st.elf" "$out/st.o" "$out/softfp.o" \
        > /dev/null 2>&1 || { echo "stress $opt: does not link"; exit 1; }
    got=$(sh "$H/run.sh" "$out/st.elf" 2>&1 | head -1)
    [ "$got" = "$swant" ] || {
        echo "stress $opt: twelve live floats disagree with the host"
        echo "  want: $swant"
        echo "  got:  $got"; exit 1; }
done
echo "twelve live floats agree with the host at four levels, so the FP
register class is placing values soundly"

# -mfpu= stays REFUSED. The FPU arithmetic works, but the flag promises
# the hard-float ABI as well, and that is not finished -- accepting it
# now would mean accepting a promise and emitting something else.
if "$EMBCC" --target=$T -mfpu=fpv4-sp-d16 -c "$out/d.c" -o /dev/null \
     2> "$out/f.err"; then
    echo "-mfpu=fpv4-sp-d16 was accepted before the hard-float ABI exists"
    exit 1
fi
grep -q 'not supported' "$out/f.err" || {
    echo "the -mfpu= refusal does not say why:"; cat "$out/f.err"; exit 1; }
echo "-mfpu= is still refused by name: the arithmetic is in, the ABI is not"
