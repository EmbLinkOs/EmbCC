#!/bin/sh
# ARMv8-M Mainline: Cortex-M33, the RTOS requirements' third target and the
# RP2350's core.
#
# It is a LEVEL on the existing Thumb target rather than a new
# enum target_arch value, and that is the decision this test pins. The enum
# keys the DATA MODEL -- D-016's reasoning for RISC-V being two targets --
# and ARMv8-M's is identical to ARMv7-M's: ILP32, the same sizes, the same
# AAPCS32, the same backend. What differs is what the object SAYS about
# itself and what the preprocessor tells the program:
#
#   Tag_CPU_arch       17 (ARM v8-M Mainline) rather than 10 or 13
#   Tag_THUMB_ISA_use  3 rather than 2 -- a wider instruction set, which is
#                      what lets a linker refuse to put a v8-M object into an
#                      image whose other parts cannot run it
#   __ARM_ARCH         8, with __ARM_ARCH_8M_MAIN__ rather than __ARM_ARCH_7M__
#
# A second enum value would have duplicated a data model to express none of
# that.
set -u
echo "TEST-MARKER thumbv8m-target"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/thumbv8m-target
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

# ---- the triple is recognised and canonical ---------------------------
for t in thumbv8m.main-none-eabi thumbv8m.main thumbv8m-none-eabi \
         armv8m.main-none-eabi; do
    got=$("$EMBCC" --target=$t -dumpmachine 2>/dev/null) || {
        echo "--target=$t is not recognised"; exit 1; }
    [ "$got" = "thumbv8m.main-none-eabi" ] || {
        echo "--target=$t canonicalises to '$got', not thumbv8m.main-none-eabi
        -- the sub-architecture has to survive, or the object reports the
        wrong one"; exit 1; }
done
# and the v7-M spellings still answer for themselves
[ "$("$EMBCC" --target=thumbv7m-none-eabi -dumpmachine)" = \
  "thumbv7m-none-eabi" ] || { echo "v7-M's canonical name changed"; exit 1; }
[ "$("$EMBCC" --target=thumbv7em-none-eabi -dumpmachine)" = \
  "thumbv7em-none-eabi" ] || { echo "v7E-M's canonical name changed"; exit 1; }

# ---- the data model is the SAME, which is why this is not a new target --
cat > "$out/dm.c" <<'CEOF'
_Static_assert(sizeof(int) == 4, "");
_Static_assert(sizeof(long) == 4, "");
_Static_assert(sizeof(void *) == 4, "");
_Static_assert(sizeof(long long) == 8, "");
_Static_assert(sizeof(double) == 8, "");
_Static_assert(sizeof(long double) == 8, "");
_Static_assert((char)-1 > 0, "char is unsigned on ARM");
int ok;
CEOF
for t in thumbv7m-none-eabi thumbv8m.main-none-eabi; do
    "$EMBCC" --target=$t -fsyntax-only "$out/dm.c" || {
        echo "$t: the data model differs from what it should be -- and if the
        two Thumb levels ever DO differ, they need two enum target_arch
        values and not one"; exit 1; }
done
echo "the data model is identical to ARMv7-M's, which is why this is a level"

# ---- the preprocessor ------------------------------------------------
v8=$("$EMBCC" --target=thumbv8m.main-none-eabi --dump-predef 2>/dev/null)
v7=$("$EMBCC" --target=thumbv7m-none-eabi --dump-predef 2>/dev/null)
echo "$v8" | grep -q '^#define __ARM_ARCH 8$' || {
    echo "__ARM_ARCH is not 8 for ARMv8-M"; exit 1; }
echo "$v8" | grep -q '^#define __ARM_ARCH_8M_MAIN__ 1$' || {
    echo "__ARM_ARCH_8M_MAIN__ is missing; a CMSIS header selects on it"
    exit 1; }
echo "$v7" | grep -q '^#define __ARM_ARCH 7$' || {
    echo "ARMv7-M's __ARM_ARCH changed"; exit 1; }
echo "$v8" | grep -q '__ARM_ARCH_7M__' && {
    echo "ARMv8-M still claims to be ARMv7-M"; exit 1; }
# The security extension is NOT advertised. clang defines __ARM_FEATURE_CMSE
# for every v8-M target because it is part of the architecture, but EmbCC
# cannot emit for it: a non-secure entry function needs the linker to mint a
# secure gateway veneer and embld does not. A header that sees the macro
# writes __attribute__((cmse_nonsecure_entry)), so leaving it in advertises a
# feature whose use fails somewhere else entirely.
echo "$v8" | grep -q '__ARM_FEATURE_CMSE' && {
    echo "__ARM_FEATURE_CMSE is defined, and EmbCC cannot emit a secure
    gateway -- a header would take that path and fail elsewhere"; exit 1; }
echo "the macros say ARMv8-M Mainline, and do not claim TrustZone"

# ---- the object says so too -------------------------------------------
printf 'int f(int a, int b) { return a * b + 1; }\n' > "$out/f.c"
"$EMBCC" --target=thumbv8m.main-none-eabi -O1 -c "$out/f.c" -o "$out/f.o" \
    2> "$out/e.txt" || {
    echo "compiling for ARMv8-M failed:"; head -4 "$out/e.txt"; exit 1; }
if command -v llvm-readobj >/dev/null 2>&1; then
    llvm-readobj -A "$out/f.o" > "$out/attrs.txt" 2>/dev/null
    grep -q "ARM v8-M Mainline" "$out/attrs.txt" || {
        echo "the object's Tag_CPU_arch is not ARM v8-M Mainline:"
        grep -A2 CPU_arch "$out/attrs.txt" | head -6; exit 1; }
    # THUMB_ISA_use 3, not 2. A linker uses it to refuse an object built for
    # a wider instruction set than the rest of the image can run.
    # llvm-readobj prints Value BEFORE TagName, so the context is behind it.
    grep -B3 "TagName: THUMB_ISA_use" "$out/attrs.txt" |
        grep -q "Value: 3" || {
        echo "Tag_THUMB_ISA_use is not 3 for ARMv8-M -- claiming 2 would let
        this object into an image built for a narrower set:"
        grep -B2 -A2 THUMB_ISA_use "$out/attrs.txt" | head -8; exit 1; }
    "$EMBCC" --target=thumbv7m-none-eabi -O1 -c "$out/f.c" -o "$out/g.o"
    llvm-readobj -A "$out/g.o" 2>/dev/null > "$out/attrs7.txt"
    grep -q "ARM v8-M Mainline" "$out/attrs7.txt" && {
        echo "an ARMv7-M object claims to be ARMv8-M"; exit 1; }
    echo "the object reports ARM v8-M Mainline and the wider Thumb set, and
an ARMv7-M object still reports itself"
fi

# ---- and it RUNS on a Cortex-M33 -------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "(SKIP: no $QEMU to run it)"; exit 0; }
"$QEMU" -M help 2>/dev/null | grep -q mps2-an505 || {
    echo "(SKIP: this $QEMU has no mps2-an505)"; exit 0; }

cat > "$out/run.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
int main(void)
{
    long i, s = 0;
    unsigned u = 0xdeadbeefu;
    long long w = 0x0102030405060708LL;
    for (i = 1; i <= 10; i++) s += i;
    putn(s);                       /* 55 */
    putn(1234L * 5678L);           /* 7006652 */
    putn(-1000000L / 7L);          /* -142857 */
    putn((long)(u >> 17));         /* 0x6f56 */
    putn((long)(w >> 40));         /* 0x010203 */
    putn((long)(int)(w & 0xffff)); /* 0x0708 */
    puts_("DONE\n");
    for (;;) ;
}
EOF
H=$out/h; mkdir -p "$H"
for O in -O0 -O1 -O2 -Os; do
    for f in boot io; do
        "$EMBCC" --target=thumbv8m.main-none-eabi $O -c \
            tests/harness/thumb-m33/$f.c -o "$H/$f.o" 2> "$out/h.err" || {
            echo "$O: the harness $f did not compile:"
            head -5 "$out/h.err"; exit 1; }
    done
    "$EMBCC" --target=thumbv8m.main-none-eabi $O -c "$out/run.c" \
        -o "$H/run.o" 2> "$out/r.err" || {
        echo "$O: the program did not compile:"; head -5 "$out/r.err"; exit 1; }
    EMBCC_M33_HARNESS="$H" sh tests/harness/thumb-m33/link.sh "$H/run.elf" \
        "$H/run.o" 2> "$out/l.err" || {
        echo "$O: link failed:"; head -4 "$out/l.err"; exit 1; }
    EMBCC_QEMU_UNTIL=DONE EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-15} \
        sh tests/harness/thumb-m33/run.sh "$H/run.elf" > "$out/got.$O" 2>/dev/null
    sed -n '1,/DONE/p' "$out/got.$O" > "$out/cut"
    want="55 7006652 -142857 28502 66051 1800 DONE"
    got=$(tr -d '\n' < "$out/cut" | sed 's/  *$//')
    [ "$got" = "$want" ] || {
        echo "$O: the Cortex-M33 disagrees."
        echo "  want: $want"
        echo "  got:  $got"
        head -c 200 "$out/got.$O" | od -c | head -4; exit 1; }
done

# ---- the runtime, which the program above never touched ---------------
#
# Everything above is integer arithmetic the backend does inline, so it would
# have passed with NO runtime -- and for a while there effectively was none:
# this target's predefined macros claimed a hardware FPU (`__ARM_FP 0xe`),
# lib/rt/softfp.c's guard compiled to an empty object, and no float program
# linked for a Cortex-M33. So the shared programs the other targets run
# (tests/golden/embedded-*.c) run here too, against the SHIPPED archive
# (tools/build-rt.sh, the recipe `make rt-embedded` uses), compared with the
# host: floats as bit patterns, 64-bit division, aggregates by value, varargs.
#
# Complex arithmetic is compared against lib/rt/complex.c built FOR THE HOST,
# not against the host's own __divsc3. Complex division is not correctly
# rounded in C and compiler-rt uses a different algorithm, so the host's
# answer differs in the last bit -- and on (1e30+1e30i)/(1e30+1e30i) it is the
# LESS accurate one, 2.2e-8 where the exact imaginary part is 0. The same
# algorithm on both sides compares two compilers rather than two runtimes.
T=thumbv8m.main-none-eabi
sh tools/build-rt.sh "$T" "$out/rt" 2> "$out/rt.err" || {
    echo "the runtime does not build for $T:"; head -3 "$out/rt.err"; exit 1; }
cat > "$out/wrap.c" <<'EOF'
void puts_(const char *s);
int prog_main(void);
int main(void) { prog_main(); puts_("<<END>>\n"); for (;;) ; }
EOF
cat > "$out/hostio.c" <<'EOF'
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) putchar(*s++); }
void putn(long v) { printf("%ld ", v); }
int prog_main(void);
int main(void) { return prog_main(); }
EOF
cat > "$out/cx.c" <<'EOF'
void writec(int c); void puts_(const char *s);
static void phex(unsigned char b)
{ static const char d[] = "0123456789abcdef"; writec(d[b >> 4]); writec(d[b & 15]); }
static void pf(float f)
{ unsigned char *p = (unsigned char *)&f; int i; for (i = 3; i >= 0; i--) phex(p[i]); writec(' '); }
static float _Complex mul(float _Complex a, float _Complex b) { return a * b; }
static float _Complex dv(float _Complex a, float _Complex b) { return a / b; }
static float _Complex mk(float r, float i)
{ float _Complex z; __real__ z = r; __imag__ z = i; return z; }
int prog_main(void)
{
    float _Complex a = mk(1.5f, -2.0f), b = mk(0.25f, 3.0f), z;
    float inf = 1e38f * 10.0f, nan = inf - inf;
    z = mul(a, b);                          pf(__real__ z); pf(__imag__ z);
    z = dv(a, b);                           pf(__real__ z); pf(__imag__ z);
    /* Annex G: an infinite operand makes the product infinite whatever the
     * other's NaNs -- the case the naive formula gets wrong */
    z = mul(mk(inf, nan), mk(2.0f, 0.0f));  pf(__real__ z); pf(__imag__ z);
    z = dv(mk(1.0f, 1.0f), mk(0.0f, 0.0f)); pf(__real__ z); pf(__imag__ z);
    /* Smith's method: no overflow in the intermediate products */
    z = dv(mk(1e30f, 1e30f), mk(1e30f, 1e30f)); pf(__real__ z); pf(__imag__ z);
    puts_("\n");
    return 0;
}
EOF
# embedded-float.c takes square roots: libc's sqrt, which has no
# instruction to be on this core either.
"$EMBCC" --target=$T -Os -Ilib/libc/include -c lib/libc/src/math/sqrt.c \
    -o "$out/sqrt.o" || { echo "libc's sqrt does not compile"; exit 1; }
for prog in float int64 aggregate varargs cx; do
    src=tests/golden/embedded-$prog.c; extra=
    [ "$prog" = cx ] && { src=$out/cx.c; extra=lib/rt/complex.c; }
    # -Dmain=prog_main, so the wrapper can print a sentinel after it returns
    # -- on the PROGRAM only, in its own compile: on one cc line with
    # hostio.c it renamed hostio.c's own main as well, and there was none.
    cc -std=c99 -w -ffp-contract=off -Dmain=prog_main -c "$src" \
       -o "$out/host-$prog.o" &&
    cc -std=c99 -w -ffp-contract=off -o "$out/host-$prog" \
       "$out/host-$prog.o" "$out/hostio.c" $extra -lm || {
        echo "$prog: the host build failed"; exit 1; }
    "$out/host-$prog" > "$out/want-$prog" || {
        echo "$prog: the host program failed"; exit 1; }
    for O in -O0 -O1 -O2 -Os; do
        for f in boot io; do
            "$EMBCC" --target=$T $O -c tests/harness/thumb-m33/$f.c \
                -o "$H/$f.o" || exit 1
        done
        "$EMBCC" --target=$T $O -c "$out/wrap.c" -o "$H/wrap.o" || exit 1
        "$EMBCC" --target=$T $O -Dmain=prog_main -c "$src" -o "$H/p.o" \
            2> "$out/p.err" || {
            echo "$prog $O: did not compile:"; head -4 "$out/p.err"; exit 1; }
        EMBCC_M33_HARNESS="$H" sh tests/harness/thumb-m33/link.sh \
            "$H/p.elf" "$H/p.o" "$H/wrap.o" "$out/sqrt.o" "$out/rt/librt.a" \
            2> "$out/l.err" || {
            echo "$prog $O: link failed:"; head -4 "$out/l.err"; exit 1; }
        EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
            sh tests/harness/thumb-m33/run.sh "$H/p.elf" 2>/dev/null \
            | sed '/<<END>>/,$d' > "$out/got-$prog$O"
        cmp -s "$out/want-$prog" "$out/got-$prog$O" || {
            echo "$prog $O: the Cortex-M33 disagrees with the host:"
            diff "$out/want-$prog" "$out/got-$prog$O" | head -6; exit 1; }
    done
done

echo "and it runs on a Cortex-M33 at four optimisation levels: 32-bit
arithmetic, a signed divide, an unsigned shift and a 64-bit shift, all
agreeing with the arithmetic they were computed from
The harness lives at the board's SECURE alias, 0x10000000. On an Armv8-M
with TrustZone the same memory is non-secure at 0x00000000 and the core
leaves reset in the SECURE state, so an image linked at 0 is fetched from
non-secure memory by a secure core -- QEMU answers \"Lockup: can't escalate
3 to HardFault\" before the first instruction retires.
The runtime runs there too, from the archive that ships: IEEE floats bit for
bit, 64-bit division, aggregates by value, varargs, and complex multiply and
divide including the Annex G infinity case and Smith's method, all agreeing
with the host at four optimisation levels."
