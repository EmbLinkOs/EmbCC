#!/bin/sh
# The IEEE-754 bit builtins: fabs, copysign, signbit and the
# isnan/isinf/isfinite/isnormal/isinf_sign family.
#
# These are checked by RUNNING them, on all four targets, at four
# optimisation levels, against answers the referee compiler produced from
# the same source -- because every interesting input here is one the
# obvious floating-point spelling gets WRONG:
#
#   fabs(-0.0)        `x < 0 ? -x : x` returns -0.0. The bits say +0.0.
#   fabs(-nan)        the same expression leaves the sign set.
#   copysign(nan,-1)  IEEE copies the sign onto a NaN too.
#   isnormal(sub)     a subnormal is finite, non-zero, and NOT normal.
#
# They are all lowered through ONE new IR op -- `bitcast`, the move
# between the register files -- and integer arithmetic after it, so this
# also checks that op on each backend. Two bugs it has already caught:
# the x86-64 and aarch64 lowerings read the float's stack SLOT, which a
# value the allocator handed to the integer file does not keep current
# (`float r = fabsf(x); memcpy(&b,&r,4)` is enough to cause that), and
# the predicates were typed `unsigned int`, so isinf_sign(-inf) widened
# to 4294967295 instead of -1 -- visible only where `long` is 64 bits.
set -u
echo "TEST-MARKER float-bits"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/float-bits
rm -rf "$out"; mkdir -p "$out"

# The bit patterns, not the printed decimals: a NaN's sign survives the
# arithmetic but not every libc's printf, so comparing "%a" output would
# compare the two C libraries instead of the two compilers.
cat > "$out/t.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);
union du { double d; unsigned long long u; };
union fu { float f; unsigned u; };
static double mk(unsigned long long b){ union du x; x.u = b; return x.d; }
static unsigned long long B(double d){ union du x; x.d = d; return x.u; }
static float mkf(unsigned b){ union fu x; x.u = b; return x.f; }
static unsigned F(float f){ union fu x; x.f = f; return x.u; }
static void ph(unsigned long long v, int nib)
{
    for (int i = nib - 1; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 15);
        writec(d < 10 ? '0' + d : 'a' + d - 10);
    }
    writec(' ');
}
int main(void)
{
    /* +0, -0, +1.5, -1.5, +inf, -inf, NaN, a subnormal */
    unsigned long long dv[] = {
        0ULL, 0x8000000000000000ULL, 0x3ff8000000000000ULL,
        0xbff8000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff8000000000000ULL, 0x0008000000000000ULL };
    for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; i++) {
        double x = mk(dv[i]);
        ph(B(__builtin_fabs(x)), 16);
        ph(B(__builtin_copysign(x, -1.0)), 16);
        putn(!!__builtin_signbit(x));   putn(!!__builtin_isnan(x));
        putn(!!__builtin_isinf(x));     putn(!!__builtin_isfinite(x));
        putn(!!__builtin_isnormal(x));  putn(__builtin_isinf_sign(x));
        writec('\n');
    }
    unsigned fv[] = { 0u, 0x80000000u, 0x40200000u, 0xc0200000u,
                      0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00400000u };
    for (unsigned i = 0; i < sizeof fv / sizeof fv[0]; i++) {
        float x = mkf(fv[i]);
        ph(F(__builtin_fabsf(x)), 8);
        ph(F(__builtin_copysignf(x, -1.0f)), 8);
        putn(!!__builtin_signbit(x));   putn(!!__builtin_isnan(x));
        putn(!!__builtin_isinf(x));     putn(!!__builtin_isfinite(x));
        putn(!!__builtin_isnormal(x));  putn(__builtin_isinf_sign(x));
        writec('\n');
    }
    puts_("\n==END==\n");
    return 0;
}
CEOF

# The hosted shim, so the SAME source can be run by the referee compiler.
cat > "$out/shim.c" <<'CEOF'
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { printf("%s", s); }
void putn(long v) { printf("%ld ", v); }
CEOF

norm() { sed 's/[[:space:]]*$//; s/==END==//; /^$/d' "$1"; }

cc -O2 -o "$out/ref" "$out/t.c" "$out/shim.c" 2>"$out/ref.err" || {
    echo "the referee compiler could not build the program:"
    head -3 "$out/ref.err"; exit 1; }
"$out/ref" > "$out/ref.raw" || { echo "the referee's program did not run"; exit 1; }
norm "$out/ref.raw" > "$out/want.txt"
[ "$(wc -l < "$out/want.txt" | tr -d ' ')" = 16 ] || {
    echo "the referee produced $(wc -l < "$out/want.txt") lines, wanted 16"
    exit 1; }

# ---- the hosted target, through the harness ---------------------------
#
# --target="$TARGET", which these two compiles were missing. t_link below
# branches on $ARCH and links with the aarch64 harness when the suite was
# invoked with --target=aarch64-elf, so without it this built x86-64 objects
# and handed them to aarch64-elf-ld: "Relocations in generic ELF (EM: 62)",
# which is the x86-64 machine number. tests/run.sh exports EMBCC_TARGET for
# exactly this and tests/lib.sh turns it into $TARGET.
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target="$TARGET" $opt -c "$out/t.c" -o "$out/h$opt.o" ||
        { echo "$opt: the program does not compile"; exit 1; }
    "$EMBCC" --target="$TARGET" $opt -c "$out/shim.c" -o "$out/s$opt.o" ||
        { echo "$opt: the shim does not compile"; exit 1; }
    t_link "$out/h$opt" "$out/h$opt.o" "$out/s$opt.o" ||
        { echo "$opt: could not link"; exit 1; }
    t_run "$out/h$opt" > "$out/h$opt.raw" 2>&1
    norm "$out/h$opt.raw" > "$out/h$opt.txt"
    cmp -s "$out/want.txt" "$out/h$opt.txt" || {
        echo "$opt: the bit builtins disagree with the referee:"
        diff "$out/want.txt" "$out/h$opt.txt" | head -6; exit 1; }
done
echo "hosted: the bit builtins agree with the referee at -O0, -O1, -O2 and -Os"

# ---- the bare-metal targets, on QEMU ----------------------------------
ran=0
for w in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$w}
    command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP rv$w: $QEMU absent"; continue; }
    ran=$((ran + 1))
    d="$out/rv$w"; mkdir -p "$d"
    export EMBCC_RISCV_HARNESS="$PWD/$d"
    for f in boot io; do
        "$EMBCC" --target=riscv$w-unknown-elf -c "tests/harness/riscv/$f.c" \
            -o "$d/$f.o" || { echo "rv$w: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=riscv$w-unknown-elf $opt -c "$out/t.c" -o "$d/a$opt.o" ||
            { echo "rv$w $opt: does not compile"; exit 1; }
        sh tests/harness/riscv/link.sh "$d/a$opt.elf" "$d/a$opt.o" ||
            { echo "rv$w $opt: could not link"; exit 1; }
        sh tests/harness/riscv/run.sh "$d/a$opt.elf" "$w" > "$d/o$opt.raw" 2>&1
        norm "$d/o$opt.raw" > "$d/o$opt.txt"
        cmp -s "$out/want.txt" "$d/o$opt.txt" || {
            echo "rv$w $opt: the bit builtins disagree with the referee:"
            diff "$out/want.txt" "$d/o$opt.txt" | head -6; exit 1; }
    done
    echo "rv$w: agrees at -O0, -O1, -O2 and -Os"
done

QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
if command -v "$QEMU" >/dev/null 2>&1; then
    ran=$((ran + 1))
    d="$out/thumb"; mkdir -p "$d"
    export EMBCC_THUMB_HARNESS="$PWD/$d"
    for f in boot io; do
        "$EMBCC" --target=thumbv7m-none-eabi -c "tests/harness/thumb/$f.c" \
            -o "$d/$f.o" || { echo "thumb: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=thumbv7m-none-eabi $opt -c "$out/t.c" -o "$d/a$opt.o" ||
            { echo "thumb $opt: does not compile"; exit 1; }
        sh tests/harness/thumb/link.sh "$d/a$opt.elf" "$d/a$opt.o" ||
            { echo "thumb $opt: could not link"; exit 1; }
        sh tests/harness/thumb/run.sh "$d/a$opt.elf" > "$d/o$opt.raw" 2>&1
        norm "$d/o$opt.raw" > "$d/o$opt.txt"
        cmp -s "$out/want.txt" "$d/o$opt.txt" || {
            echo "thumb $opt: the bit builtins disagree with the referee:"
            diff "$out/want.txt" "$d/o$opt.txt" | head -6; exit 1; }
    done
    echo "thumb: agrees at -O0, -O1, -O2 and -Os"
else
    echo "SKIP thumb: $QEMU absent"
fi
[ "$ran" -gt 0 ] || echo "SKIP the bare-metal half: no qemu"

# ---- what it REFUSES --------------------------------------------------
# A 16-byte long double's sign and exponent are past one register; they
# go through memory (irgen fb_wide), which tests/exec/fp-bits-long-double.c
# runs. Here only that it compiles where it was once refused.
printf 'int f(long double x){ return __builtin_signbit(x); }\n' > "$out/ld.c"
"$EMBCC" -c "$out/ld.c" -o /dev/null 2> "$out/ld.err" || {
    echo "signbit on a 16-byte long double was refused:"; cat "$out/ld.err"
    exit 1; }
# An integer argument to a predicate is a missing cast, not a question.
printf 'int f(int x){ return __builtin_isnan(x); }\n' > "$out/int.c"
if "$EMBCC" -fsyntax-only "$out/int.c" 2> "$out/int.err"; then
    echo "isnan of an int was accepted"; exit 1
fi
echo "a 16-byte long double compiles, and an integer argument is refused by name"
