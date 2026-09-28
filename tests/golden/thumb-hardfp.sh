#!/bin/sh
# -mfloat-abi=hard: floating point PASSED AND RETURNED in the FPU's
# registers (AAPCS §6.1.2, "VFP"), on a Cortex-M4F and a Cortex-M33.
#
# The convention is only worth having if it is the SAME one every other
# toolchain uses, since its point is linking with code EmbCC did not
# compile. So the evidence here is a cross test against clang, in both
# directions, on the part: tests/golden/hardfp/callee.c hashes the bit
# pattern of every argument it receives, caller.c prints the hashes and
# every kind of result, and each pairing of EmbCC and clang as caller and
# callee must print what the host prints. Between them the two files reach
# every rule the convention has -- back-fill, the s0-s15 and d0-d7 limits,
# homogeneous aggregates of floats and of doubles, `_Complex`, aggregates
# that are NOT homogeneous, the VFP file closing once anything spills,
# C.5's no-split rule, and variadic functions staying on the base
# convention -- and each of those, broken on purpose in the placer, fails
# it.
set -u
echo "TEST-MARKER thumb-hardfp"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
"$QEMU" -machine help 2>/dev/null | grep -q mps2-an386 || {
    echo "SKIP: this QEMU has no mps2-an386 (a Cortex-M4 with an FPU)"
    exit 0; }

D=$EMBCC_ROOT/tests/golden/hardfp
out=tests/golden/out/thumb-hardfp
rm -rf "$out"; mkdir -p "$out"

cc -w -o "$out/host" "$D/caller.c" "$D/callee.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null ||
    { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -2 | tr '\n' '|')
cc -w -o "$out/ahost" "$D/arith.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null ||
    { echo "the arithmetic host reference does not build"; exit 1; }
awant=$("$out/ahost" | head -1)

CLANG=${EMBCC_CLANG:-clang}
have_clang=0
command -v "$CLANG" >/dev/null 2>&1 && have_clang=1

# One board at a time: its triple, its CPU and FPU as clang spells them,
# its harness directory, and the variable that harness reads.
board() {
    T=$1 CPU=$2 FPU=$3 HD=$EMBCC_ROOT/tests/harness/$4 HV=$5
    B=$out/$4; mkdir -p "$B"
    eval "$HV=\$PWD/\$B; export $HV"
    for f in boot io; do
        "$EMBCC" --target=$T -c "$HD/$f.c" -o "$B/$f.o" || {
            echo "$T: the harness does not compile"; exit 1; }
    done
    "$EMBCC" --target=$T -Os -c lib/rt/softfp.c -o "$B/softfp.o" || {
        echo "$T: the runtime does not compile"; exit 1; }
    CL="$CLANG --target=$T -mcpu=$CPU -mfpu=$FPU -mfloat-abi=hard
        -ffreestanding -fno-builtin -O2 -c"

    pairs="e:e"
    [ $have_clang = 1 ] && pairs="e:e e:c c:e"
    for pair in $pairs; do
        er=${pair%:*} ee=${pair#*:}
        for opt in -O0 -O1 -O2 -Os; do
            if [ $er = e ]; then
                "$EMBCC" --target=$T $opt -c "$D/caller.c" -o "$B/caller.o"
            else
                $CL "$D/caller.c" -o "$B/caller.o"
            fi || { echo "$T caller=$er $opt: does not compile"; exit 1; }
            if [ $ee = e ]; then
                "$EMBCC" --target=$T $opt -c "$D/callee.c" -o "$B/callee.o"
            else
                $CL "$D/callee.c" -o "$B/callee.o"
            fi || { echo "$T callee=$ee $opt: does not compile"; exit 1; }
            sh "$HD/link.sh" "$B/t.elf" "$B/caller.o" "$B/callee.o" \
                "$B/softfp.o" > "$B/ln.log" 2>&1 || {
                echo "$T caller=$er callee=$ee $opt: does not link"
                head -3 "$B/ln.log"; exit 1; }
            got=$(sh "$HD/run.sh" "$B/t.elf" 2>&1 | head -2 | tr '\n' '|')
            [ "$got" = "$want" ] || {
                echo "$T caller=$er callee=$ee $opt: disagrees with the host"
                echo "  want: $want"; echo "  got:  $got"; exit 1; }
            # clang's half does not change with EmbCC's -O level
            [ $er = c ] || [ $ee = c ] && break
        done
    done

    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/arith.c" -o "$B/arith.o" &&
        sh "$HD/link.sh" "$B/a.elf" "$B/arith.o" "$B/softfp.o" \
            > "$B/aln.log" 2>&1 || {
            echo "$T arith $opt: does not build"; head -3 "$B/aln.log"
            exit 1; }
        got=$(sh "$HD/run.sh" "$B/a.elf" 2>&1 | head -1)
        [ "$got" = "$awant" ] || {
            echo "$T arith $opt: disagrees with the host"
            echo "  want: $awant"; echo "  got:  $got"; exit 1; }
    done
}

board thumbv7em-none-eabihf cortex-m4 fpv4-sp-d16 thumb-m4f EMBCC_THUMB_HARNESS
if [ $have_clang = 1 ]; then
    echo "Cortex-M4F: EmbCC and clang agree with the host as caller and callee,
either way round, at four levels, on every rule of AAPCS-VFP"
else
    echo "Cortex-M4F: EmbCC agrees with the host at four levels (clang absent,
so the cross pairings were not run)"
fi
echo "and float and double arithmetic across calls is bit-identical to the
host: floats on the FPU, doubles through the runtime in core registers"

if "$QEMU" -machine help 2>/dev/null | grep -q mps2-an505; then
    board thumbv8m.main-none-eabihf cortex-m33 fpv5-sp-d16 thumb-m33 \
        EMBCC_M33_HARNESS
    echo "Cortex-M33 (FPv5-SP-D16): the same, on the other part"
else
    echo "SKIP the Cortex-M33 half: this QEMU has no mps2-an505"
fi

# ---- the name, the flags, and the objects agree ------------------------
B=$out/thumb-m4f
[ "$("$EMBCC" --target=thumbv7em-none-eabi -mfpu=fpv4-sp-d16 \
      -mfloat-abi=hard -dumpmachine)" = thumbv7em-none-eabihf ] || {
    echo "-mfloat-abi=hard does not report the -eabihf triple"; exit 1; }
[ "$("$EMBCC" --target=thumbv7em-none-eabihf -mfloat-abi=soft \
      -dumpmachine)" = thumbv7em-none-eabi ] || {
    echo "-mfloat-abi=soft does not override an -eabihf triple"; exit 1; }
"$EMBCC" --target=thumbv7em-none-eabihf --dump-predef > "$B/pd.txt"
grep -q '__ARM_PCS_VFP 1' "$B/pd.txt" && ! grep -q '__ARM_PCS ' "$B/pd.txt" &&
    ! grep -q '__SOFTFP__' "$B/pd.txt" || {
    echo "the hard-float predefined macros are wrong:"; cat "$B/pd.txt"
    exit 1; }
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" --arch-specific "$B/callee.o" | grep -A2 'TagName: ABI_VFP_args' |
        grep -q 'AAPCS VFP' || {
        echo "a hard-float object does not say it passes floats in VFP
registers"; exit 1; }
fi
# ...which is what lets the linker refuse the mix that CANNOT work.
"$EMBCC" --target=thumbv7em-none-eabi -c "$D/callee.c" -o "$B/soft.o"
if sh "$EMBCC_ROOT/tests/harness/thumb-m4f/link.sh" "$B/mix.elf" \
     "$B/caller.o" "$B/soft.o" "$B/softfp.o" > "$B/mix.log" 2>&1; then
    echo "embld linked a hard-float caller with a soft-float callee"; exit 1
fi
echo "-mfloat-abi=hard and the -eabihf triple are one thing, say so in
-dumpmachine, the macros and the attributes, and will not link with
soft-float code"

# ---- pcs("aapcs"): the base convention on request -----------------------
# What the runtime uses to keep its helpers on core registers; a user
# declaration of such a helper must reach the same registers.
# Run against clang in both directions: clang defines one base-convention
# function and calls one EmbCC defines, and each returns its SECOND
# argument -- so reading the operands from s0/s1 instead of r0/r1 returns
# the wrong one, or garbage.
if [ $have_clang = 1 ]; then
    cat > "$B/pcs_e.c" <<'CEOF'
void writec(int c); void puts_(const char *s);
__attribute__((pcs("aapcs"))) float c_sel(float a, float b);
__attribute__((pcs("aapcs"))) double c_seld(double a, double b);
float c_call_e(float a, float b);
__attribute__((pcs("aapcs"))) float e_sel(float a, float b) { (void)a; return b; }
static void hx(unsigned v)
{ for (int i = 28; i >= 0; i -= 4) writec("0123456789abcdef"[(v >> i) & 15]);
  writec(' '); }
union F { float f; unsigned u; };
union D { double d; unsigned w[2]; };
int main(void)
{
    union F x; union D y;
    x.f = c_sel(1.5f, 2.5f); hx(x.u);
    y.d = c_seld(3.5, 4.75); hx(y.w[0]); hx(y.w[1]);
    x.f = c_call_e(5.5f, 6.25f); hx(x.u);
    puts_("\n==END==\n");
    return 0;
}
CEOF
    cat > "$B/pcs_c.c" <<'CEOF'
__attribute__((pcs("aapcs"))) float c_sel(float a, float b) { (void)a; return b; }
__attribute__((pcs("aapcs"))) double c_seld(double a, double b) { (void)a; return b; }
__attribute__((pcs("aapcs"))) float e_sel(float a, float b);
float c_call_e(float a, float b) { return e_sel(a, b); }
CEOF
    "$EMBCC" --target=thumbv7em-none-eabihf -O2 -c "$B/pcs_e.c" \
        -o "$B/pcs_e.o" &&
    $CLANG --target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 \
        -mfloat-abi=hard -ffreestanding -O2 -c "$B/pcs_c.c" -o "$B/pcs_c.o" &&
    EMBCC_THUMB_HARNESS=$PWD/$B sh "$EMBCC_ROOT/tests/harness/thumb-m4f/link.sh" \
        "$B/pcs.elf" "$B/pcs_e.o" "$B/pcs_c.o" > "$B/pcs.log" 2>&1 || {
        echo "the pcs cross test does not build"; cat "$B/pcs.log"; exit 1; }
    got=$(sh "$EMBCC_ROOT/tests/harness/thumb-m4f/run.sh" "$B/pcs.elf" |
          head -1)
    [ "$got" = "40200000 00000000 40130000 40c80000 " ] || {
        echo "pcs(\"aapcs\") disagrees with clang: got '$got'"; exit 1; }
    echo "pcs(\"aapcs\") calls and is called by clang's, both directions"
fi
refuse() {
    printf '%s\n' "$2" > "$B/r.c"
    if "$EMBCC" --target=${3:-thumbv7em-none-eabihf} -c "$B/r.c" \
         -o /dev/null 2> "$B/r.err"; then
        echo "accepted, and must not be: $1"; exit 1
    fi
    grep -q "$4" "$B/r.err" || {
        echo "refused without the reason ($1):"; cat "$B/r.err"; exit 1; }
}
refuse "the address of a base-convention function" \
    '__attribute__((pcs("aapcs"))) float g(float); float (*p)(float) = g;' \
    "" "taking the address"
refuse "pcs on a variable" '__attribute__((pcs("aapcs"))) int v;' "" \
    "only supported on a function"
refuse "two conventions for one function" \
    '__attribute__((pcs("aapcs"))) float g(float);
__attribute__((pcs("aapcs-vfp"))) float g(float);' "" "different pcs"
refuse "pcs away from ARM" '__attribute__((pcs("aapcs"))) void g(void);' \
    riscv32-unknown-elf "not an ARM target"
refuse "an unknown convention" '__attribute__((pcs("fast"))) void g(void);' \
    "" "aapcs"
echo "pcs(\"aapcs\") moves floats to the core registers at the call, and is
refused by name wherever it could be dropped"
