#!/bin/sh
# `double` in hardware on the Cortex-M7: -mfpu=fpv5-d16, the double-precision
# unit, and the code the backend makes for it, RUN on a Cortex-M7.
#
# FPv4-SP-D16 (a Cortex-M4F) and FPv5-SP-D16 (a Cortex-M33) compute in single
# precision only, so there a double is a pair of core words handed to
# __adddf3. FPv5-D16 computes doubles too, and with it every double operation
# but the 64-bit integer conversions is one VFP .f64 instruction, on doubles
# that live in d8-d15. This test holds that to four things:
#
#   1. the flags: -mfpu=fpv5-d16, and -mcpu=cortex-m7 with an -eabihf triple,
#      select the unit; the predefined macros and the object's build
#      attributes say so as clang says it; and the unit is refused BY NAME on
#      a part that does not have it.
#   2. the shape: double arithmetic, comparisons, square roots, fabs,
#      negation and the 32-bit and width conversions are .f64 instructions
#      and call no soft-float helper; doubles are passed in d0-d7 and never
#      cross to the core registers on the way.
#   3. the answers, on QEMU's mps2-an500 (a Cortex-M7 with the unit) at
#      -O0, -O1, -O2 and -Os, bit for bit against the host -- built with
#      -ffp-contract=off, because clang on arm64 fuses a*b+c into one
#      rounding by default and the M7 code here never does.
#   4. the calling convention, against clang for the same part in both
#      directions (tests/golden/hardfp, every rule of AAPCS-VFP), and the
#      softfp one, against soft-float objects in all four pairings.
#
# The rest of tests/exec on this board is tests/golden/thumb-m7-exec.sh.
set -u
echo "TEST-MARKER thumb-m7-dp"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/thumb-m7-dp
rm -rf "$out"; mkdir -p "$out"
T=thumbv7em-none-eabihf
D=$EMBCC_ROOT/tests/golden/thumb-m7-dp
HF=$EMBCC_ROOT/tests/golden/hardfp
H=$EMBCC_ROOT/tests/harness/thumb-m7
CLANG=${EMBCC_CLANG:-clang}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
have() { command -v "$1" >/dev/null 2>&1; }

# ---- 1. the flags ---------------------------------------------------------
pd() { "$EMBCC" "$@" --dump-predef 2>&1; }
for spell in "--target=$T -mcpu=cortex-m7" "--target=$T -mfpu=fpv5-d16" \
             "--target=thumbv7em-none-eabi -mfpu=fpv5-d16 -mfloat-abi=hard" \
             "--target=thumbv7em-none-eabi -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=softfp"
do
    # shellcheck disable=SC2086
    m=$(pd $spell)
    echo "$m" | grep -q '^#define __ARM_FP 0xc$' &&
    echo "$m" | grep -q '^#define __ARM_FPV5__ 1$' &&
    ! echo "$m" | grep -q '__SOFTFP__' || {
        echo "'$spell' does not select the double-precision unit:"
        echo "$m" | grep -E '__ARM_FP|SOFTFP|FPV'; exit 1; }
done
# ...and the M4F is still what the -eabihf triple means without -mcpu
pd --target=$T | grep -q '^#define __ARM_FP 0x4$' || {
    echo "thumbv7em-none-eabihf alone no longer means the Cortex-M4F's unit"
    exit 1; }
# __ARM_FP is 0xc where clang says 0xe (bit 1 is half-precision conversion,
# which EmbCC does not emit) and __ARM_FEATURE_FMA is absent (EmbCC emits no
# fused multiply-add): the rest of the FPU's macros are clang's. Except one
# clang adds that GCC does not: __ARM_PCS beside __ARM_PCS_VFP under the
# hard-float convention, where EmbCC says the VFP one alone, as GCC does
# (tests/golden/thumb-hardfp.sh holds it to that).
if have "$CLANG"; then
    for abi in hard softfp; do
        ct=$T; [ $abi = softfp ] && ct=thumbv7em-none-eabi
        nopcs='^$'; [ $abi = hard ] && nopcs='__ARM_PCS 1'
        "$CLANG" --target=$ct -mcpu=cortex-m7 -mfpu=fpv5-d16 \
            -mfloat-abi=$abi -dM -E - </dev/null 2>/dev/null |
            grep -E '__ARM_(FP|VFPV|FPV|PCS)|__VFP_FP__|__SOFTFP__' |
            grep -v "__ARM_FP16\|__ARM_FP 0xe\|$nopcs" | sort \
            > "$out/clang-$abi.mac"
        "$EMBCC" --target=$ct -mcpu=cortex-m7 -mfpu=fpv5-d16 \
            -mfloat-abi=$abi --dump-predef |
            grep -E '__ARM_(FP|VFPV|FPV|PCS)|__VFP_FP__|__SOFTFP__' |
            grep -v '__ARM_FP16\|__ARM_FP 0xc' | sort > "$out/embcc-$abi.mac"
        diff -u "$out/clang-$abi.mac" "$out/embcc-$abi.mac" > "$out/mac.diff" || {
            echo "-mfloat-abi=$abi: the FPU's macros are not clang's:"
            cat "$out/mac.diff"; exit 1; }
    done
fi
refuse() {          # refuse WHY "FLAGS" PATTERN
    # shellcheck disable=SC2086
    if "$EMBCC" $2 -c -x c /dev/null -o /dev/null 2> "$out/r.err"; then
        echo "accepted, and must not be: $1"; exit 1
    fi
    grep -q "$3" "$out/r.err" || {
        echo "refused without the reason ($1):"; cat "$out/r.err"; exit 1; }
}
refuse "the double-precision unit on a Cortex-M3" \
    "--target=thumbv7m-none-eabi -mfpu=fpv5-d16 -mfloat-abi=hard" "cortex-m7"
refuse "the double-precision unit on a Cortex-M4" \
    "--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv5-d16 -mfloat-abi=hard" \
    "does not have it"
refuse "the double-precision unit on ARMv8-M" \
    "--target=thumbv8m.main-none-eabi -mfpu=fpv5-d16 -mfloat-abi=hard" \
    "fpv5-sp-d16"
refuse "the Cortex-M33's unit on ARMv7E-M" \
    "--target=thumbv7em-none-eabi -mfpu=fpv5-sp-d16 -mfloat-abi=hard" "fpv5-d16"
# What the object says: FPv5 (Tag_FP_arch 8) with both precisions -- which
# is Tag_ABI_HardFP_use left at its default, as clang leaves it -- and the
# convention the ABI flag chose. The two FPU tags are compared with clang's;
# Tag_ABI_VFP_args on its own, because EmbCC states the base convention
# where clang leaves the tag at that default (tests/golden/thumb-fpu.sh).
printf 'double f(double a, double b) { return a * b; }\n' > "$out/a.c"
if have "$RE"; then
    for abi in hard softfp; do
        ct=$T; [ $abi = softfp ] && ct=thumbv7em-none-eabi
        "$EMBCC" --target=$ct -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=$abi \
            -c "$out/a.c" -o "$out/a-$abi.o" || exit 1
        tags() {
            "$RE" --arch-specific "$1" | grep -A3 'TagName: \(FP_arch\|ABI_HardFP_use\)' |
                grep -E 'TagName|Description' | tr -s ' '
        }
        tags "$out/a-$abi.o" > "$out/tags-embcc-$abi.txt"
        grep -q 'ARMv8-a FP-D16' "$out/tags-embcc-$abi.txt" &&
        ! grep -q 'ABI_HardFP_use' "$out/tags-embcc-$abi.txt" || {
            echo "-mfloat-abi=$abi: the object does not say FPv5-D16:"
            cat "$out/tags-embcc-$abi.txt"; exit 1; }
        conv='Description: AAPCS VFP$'; [ $abi = softfp ] && conv='Description: AAPCS$'
        "$RE" --arch-specific "$out/a-$abi.o" | grep -A2 'TagName: ABI_VFP_args' |
            grep -q "$conv" || {
            echo "-mfloat-abi=$abi: Tag_ABI_VFP_args is not $conv"; exit 1; }
        if have "$CLANG"; then
            "$CLANG" --target=$ct -mcpu=cortex-m7 -mfpu=fpv5-d16 \
                -mfloat-abi=$abi -c "$out/a.c" -o "$out/c-$abi.o" &&
            tags "$out/c-$abi.o" > "$out/tags-clang-$abi.txt" &&
            diff -u "$out/tags-clang-$abi.txt" "$out/tags-embcc-$abi.txt" || {
                echo "-mfloat-abi=$abi: the FP attributes are not clang's"
                exit 1; }
        fi
    done
fi
echo "-mfpu=fpv5-d16 and -mcpu=cortex-m7 select the double-precision unit, the
macros and attributes say so, and it is refused by name on the M3, M4 and M33"

# ---- 2. the shape ---------------------------------------------------------
if have "$OD"; then
    "$EMBCC" --target=$T -mcpu=cortex-m7 -O2 -c "$D/shape.c" -o "$out/shape.o" ||
        { echo "shape.c does not compile"; exit 1; }
    "$OD" -d --triple=thumbv7em --mattr=+fp-armv8d16 "$out/shape.o" \
        > "$out/shape.dis"
    "$OD" -dr "$out/shape.o" > "$out/shape.rel"
    for want in vadd.f64 vsub.f64 vmul.f64 vdiv.f64 vneg.f64 vabs.f64 \
                vsqrt.f64 vcmpe.f64 vcmp.f64 vcvt.f64.s32 vcvt.f64.u32 \
                vcvt.s32.f64 vcvt.u32.f64 vcvt.f64.f32 vcvt.f32.f64; do
        grep -q "	$want	" "$out/shape.dis" || {
            echo "no $want in the double-precision build"; exit 1; }
    done
    if grep -E '__(add|sub|mul|div|neg)df3|__(eq|ne|lt|le|gt|ge|unord)df2|__(float|floatun)sidf|__fix(uns)?dfsi|__extendsfdf2|__truncdfsf2|[^_]sqrt' \
           "$out/shape.rel"; then
        echo "the double-precision build still calls a soft-float helper"
        exit 1
    fi
    for need in __fixdfdi __floatdidf; do
        grep -q "$need" "$out/shape.rel" || {
            echo "the 64-bit conversion $need is not the runtime's call"
            exit 1; }
    done
    # Passing: d0, s2 and d2 are written before the call, d0 read after
    # it, and no double crosses to the core registers anywhere but the
    # two 64-bit conversions' own setup.
    awk '/<caller>:/,/^$/' "$out/shape.dis" > "$out/caller.dis"
    sed -n '1,/	bl	/p' "$out/caller.dis" > "$out/pre.dis"
    grep -q 'vmov.f64	d0, ' "$out/pre.dis" &&
    grep -q '	s2, ' "$out/pre.dis" &&
    grep -q 'vmov.f64	d2, ' "$out/pre.dis" &&
    sed -n '/	bl	/,$p' "$out/caller.dis" | grep -q ', d0$' || {
        echo "caller() does not pass and return its doubles in d0, s2, d2:"
        cat "$out/caller.dis"; exit 1; }
    awk '/<to_ll>:|<from_ll>:/{skip=1} /^$/{skip=0} !skip' "$out/shape.dis" |
        grep -E 'vmov	(r[0-9]+, r[0-9]+, d|d[0-9]+, r)' && {
        echo "a double crossed to the core registers"; exit 1; }
    echo "double arithmetic, comparisons, sqrt, fabs and the conversions are
VFP .f64 instructions, and doubles are passed in d0-d7"
else
    echo "SKIP the shape half: no $OD"
fi

# ---- the runtime: only what the unit cannot do ----------------------------
"$EMBCC" --target=$T -mcpu=cortex-m7 -Os -c lib/rt/softfp.c -o "$out/softfp7.o" ||
    { echo "lib/rt/softfp.c does not compile for the M7"; exit 1; }
if have llvm-nm; then
    got=$(llvm-nm "$out/softfp7.o" | awk '$2 == "T" { print $3 }' | sort | tr '\n' ' ')
    [ "$got" = "__fixdfdi __fixsfdi __fixunsdfdi __fixunssfdi __floatdidf __floatdisf __floatundidf __floatundisf " ] || {
        echo "lib/rt/softfp.c for the M7 defines: $got"; exit 1; }
    echo "lib/rt/softfp.c keeps only the 64-bit integer conversions on the M7"
fi

# ---- 3. the answers --------------------------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
have "$QEMU" || { echo "SKIP the rest: $QEMU absent"; exit 0; }
"$QEMU" -machine help 2>/dev/null | grep -q mps2-an500 || {
    echo "SKIP the rest: this QEMU has no mps2-an500 (a Cortex-M7)"; exit 0; }
EMBCC_M7_HARNESS=$PWD/$out; export EMBCC_M7_HARNESS
for f in boot io; do
    "$EMBCC" --target=$T -mcpu=cortex-m7 -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the M7 harness does not compile"; exit 1; }
done
"$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$out/int64.o" &&
"$EMBCC" --target=$T -mcpu=cortex-m7 -Os -Ilib/libc/include \
    -c lib/libc/src/math/sqrt.c -o "$out/sqrt.o" &&
"$EMBCC" --target=thumbv7em-none-eabi -Os -c lib/rt/softfp.c \
    -o "$out/softfp-soft.o" || { echo "the runtime does not compile"; exit 1; }

# run NAME OBJ...: link, run to the sentinel, the output in $out/NAME.txt
run() {
    name=$1; shift
    sh "$H/link.sh" "$out/$name.elf" "$@" > "$out/$name.ln" 2>&1 || {
        echo "$name: does not link"; head -3 "$out/$name.ln"; return 1; }
    EMBCC_QEMU_UNTIL='==END==' sh "$H/run.sh" "$out/$name.elf" |
        sed -n '1,/==END==/p' > "$out/$name.txt"
    grep -q '==END==' "$out/$name.txt" || {
        echo "$name: the image did not reach the end:"; head -5 "$out/$name.txt"
        return 1; }
}
host_ref() {        # host_ref NAME SRC... -> $out/NAME.want
    name=$1; shift
    cc -std=c99 -w -ffp-contract=off -o "$out/$name.host" "$@" \
        "$EMBCC_ROOT/tests/harness/thumb/hostio.c" -lm 2>/dev/null ||
        { echo "the host reference $name does not build"; exit 1; }
    "$out/$name.host" | sed -n '1,/==END==/p' > "$out/$name.want"
}
same() {            # same WANT GOT WHAT
    diff -u "$1" "$2" > "$out/last.diff" || {
        echo "$3 disagrees:"; head -16 "$out/last.diff"; exit 1; }
}

host_ref dp "$D/dp.c"
host_ref float tests/golden/embedded-float.c
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T -mcpu=cortex-m7 $opt -c "$D/dp.c" -o "$out/dp.o" ||
        { echo "dp.c $opt: does not compile"; exit 1; }
    run dp$opt "$out/dp.o" "$out/softfp7.o" || exit 1
    same "$out/dp.want" "$out/dp$opt.txt" "dp.c at $opt"
    "$EMBCC" --target=$T -mcpu=cortex-m7 $opt -c tests/golden/embedded-float.c \
        -o "$out/fl.o" || { echo "embedded-float.c $opt: does not compile"; exit 1; }
    run fl$opt "$out/fl.o" "$out/softfp7.o" "$out/int64.o" "$out/sqrt.o" || exit 1
    same "$out/float.want" "$out/fl$opt.txt" "embedded-float.c at $opt"
done
# The core-register convention with the same unit (-mfloat-abi=softfp): the
# arithmetic is the FPU's, and every double crosses to r0-r3 at a call. The
# harness is built for that convention too, in its own directory -- embld
# will not link a hard-float boot.o with it, which is the point of the tag.
mkdir -p "$out/soft"
for f in boot io; do
    "$EMBCC" --target=thumbv7em-none-eabi -c "$H/$f.c" -o "$out/soft/$f.o" ||
        { echo "the softfp harness does not compile"; exit 1; }
done
EMBCC_M7_HARNESS=$PWD/$out/soft
for opt in -O0 -O2; do
    "$EMBCC" --target=thumbv7em-none-eabi -mcpu=cortex-m7 -mfpu=fpv5-d16 \
        -mfloat-abi=softfp $opt -c "$D/dp.c" -o "$out/dps.o" ||
        { echo "dp.c softfp $opt: does not compile"; exit 1; }
    run dps$opt "$out/dps.o" "$out/softfp-soft.o" || exit 1
    same "$out/dp.want" "$out/dps$opt.txt" "dp.c with -mfloat-abi=softfp at $opt"
done
EMBCC_M7_HARNESS=$PWD/$out
echo "doubles computed on the Cortex-M7 are bit-identical to the host at -O0,
-O1, -O2 and -Os, under the hard-float and the softfp convention"

# hardfp/arith.c and cmpcvt.c: the floats and doubles of the hard-float
# test, on the part that computes both
for prog in arith cmpcvt; do
    cc -w -ffp-contract=off -o "$out/$prog.host" "$HF/$prog.c" \
        "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null ||
        { echo "the $prog host reference does not build"; exit 1; }
    "$out/$prog.host" | sed -n '1,/==END==/p' > "$out/$prog.want"
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T -mcpu=cortex-m7 $opt -c "$HF/$prog.c" \
            -o "$out/$prog.o" || { echo "$prog $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$out/$prog.elf" "$out/$prog.o" "$out/softfp7.o" \
            > "$out/$prog.ln" 2>&1 || { echo "$prog $opt: no link"; exit 1; }
        EMBCC_QEMU_UNTIL='==END==' sh "$H/run.sh" "$out/$prog.elf" |
            sed -n '1,/==END==/p' > "$out/$prog$opt.txt"
        same "$out/$prog.want" "$out/$prog$opt.txt" "hardfp/$prog.c at $opt"
    done
done
echo "and so are hardfp/arith.c and cmpcvt.c's floats and doubles"

# ---- 4. the calling convention, against clang --------------------------
cc -w -o "$out/hf.host" "$HF/caller.c" "$HF/callee.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null ||
    { echo "the hardfp host reference does not build"; exit 1; }
"$out/hf.host" | head -2 > "$out/hf.want"
CL="$CLANG --target=$T -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard
    -ffreestanding -fno-builtin -O2 -c"
pairs="e:e"
have "$CLANG" && pairs="e:e e:c c:e"
for pair in $pairs; do
    er=${pair%:*} ee=${pair#*:}
    for opt in -O0 -O1 -O2 -Os; do
        for side in caller callee; do
            who=$er; [ $side = callee ] && who=$ee
            if [ $who = e ]; then
                "$EMBCC" --target=$T -mcpu=cortex-m7 $opt -c "$HF/$side.c" \
                    -o "$out/$side.o"
            else
                $CL "$HF/$side.c" -o "$out/$side.o"
            fi || { echo "$side ($who) $opt: does not compile"; exit 1; }
        done
        sh "$H/link.sh" "$out/hf.elf" "$out/caller.o" "$out/callee.o" \
            "$out/softfp7.o" > "$out/hf.ln" 2>&1 || {
            echo "caller=$er callee=$ee $opt: does not link"
            head -3 "$out/hf.ln"; exit 1; }
        EMBCC_QEMU_UNTIL='==END==' sh "$H/run.sh" "$out/hf.elf" | head -2 \
            > "$out/hf$er$ee$opt.txt"
        same "$out/hf.want" "$out/hf$er$ee$opt.txt" \
            "AAPCS-VFP, caller=$er callee=$ee at $opt"
    done
done
[ "$pairs" = "e:e" ] && echo "(clang absent: EmbCC called only itself)"
echo "EmbCC and clang for the Cortex-M7 call each other on every rule of
AAPCS-VFP, either way round, at four levels"

# softfp against soft float: the same objects tests/golden/thumb-fpu.sh pairs,
# with doubles crossing between the M7's d registers and r0-r3.
cat > "$out/lib.c" <<'CEOF'
double scale(double a, double b, int k) { return a * b + (double)k; }
double widen(float a) { return (double)a * 3.0; }
double pick(double a, double b, double c, double d, double e)
{ return (a - b) * c + d / e; }
CEOF
cat > "$out/use.c" <<'CEOF'
void writec(int c); void puts_(const char *s);
double scale(double, double, int); double widen(float);
double pick(double, double, double, double, double);
static void hx(unsigned long long v) { int i; for (i = 60; i >= 0; i -= 4) writec("0123456789abcdef"[(v >> i) & 15]); writec(' '); }
static unsigned long long db(double d) { union { double d; unsigned long long u; } x; x.d = d; return x.u; }
int main(void)
{
    hx(db(scale(1.5, 2.25, 3)));
    hx(db(widen(0.1f)));
    hx(db(pick(10.0, 2.5, 0.125, 7.0, 3.0)));
    puts_("\n==END==\n");
    return 0;
}
CEOF
cc -w -ffp-contract=off -o "$out/ihost" "$out/lib.c" "$out/use.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null ||
    { echo "the interop host reference does not build"; exit 1; }
"$out/ihost" | head -1 > "$out/i.want"
SFP="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=softfp"
for caller in soft softfp; do
    for callee in soft softfp; do
        fl() { [ "$1" = softfp ] && echo "$SFP"; }
        # shellcheck disable=SC2046
        "$EMBCC" --target=thumbv7em-none-eabi $(fl $callee) -O2 -c "$out/lib.c" \
            -o "$out/lib.o" &&
        # shellcheck disable=SC2046
        "$EMBCC" --target=thumbv7em-none-eabi $(fl $caller) -O2 -c "$out/use.c" \
            -o "$out/use.o" || { echo "interop: does not compile"; exit 1; }
        EMBCC_M7_HARNESS=$PWD/$out/soft sh "$H/link.sh" "$out/i.elf" \
            "$out/use.o" "$out/lib.o" "$out/softfp-soft.o" > "$out/i.ln" 2>&1 || {
            echo "interop caller=$caller callee=$callee: does not link"
            head -3 "$out/i.ln"; exit 1; }
        EMBCC_QEMU_UNTIL='==END==' sh "$H/run.sh" "$out/i.elf" | head -1 \
            > "$out/i-$caller-$callee.txt"
        same "$out/i.want" "$out/i-$caller-$callee.txt" \
            "softfp interop caller=$caller callee=$callee"
    done
done
echo "softfp objects for the M7 and soft-float ones call each other in all
four pairings: the FPU does the arithmetic, r0-r3 carry the doubles"

# Against clang on the target, for the program that uses `long` (whose size
# the host does not share): the same source, linker and board.
if have "$CLANG"; then
    "$CLANG" --target=$T -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
        -ffreestanding -Os -c tests/golden/embedded-stress.c -o "$out/st-ref.o" &&
    run st-ref "$out/st-ref.o" || exit 1
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T -mcpu=cortex-m7 $opt -c tests/golden/embedded-stress.c \
            -o "$out/st.o" || { echo "embedded-stress $opt: does not compile"; exit 1; }
        run st$opt "$out/st.o" "$out/softfp7.o" "$out/int64.o" || exit 1
        same "$out/st-ref.txt" "$out/st$opt.txt" "embedded-stress.c at $opt"
    done
    echo "embedded-stress.c agrees with clang's build on the Cortex-M7"
fi
