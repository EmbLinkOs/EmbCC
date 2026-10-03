#!/bin/sh
# EmbCC's libc classifies a long double as a long double.
#
# <math.h> sent every argument wider than a float to __fpclassifyd, so a
# long double was converted to double first: on x86-64 (80-bit) and
# AArch64 (binary128), isinf(LDBL_MAX) was 1, isfinite was 0, and a long
# double subnormal was FP_ZERO. fabsl(-0.0L) was -0.0L, and copysignl
# kept a negative zero's sign when asked for a positive one.
#
# Run on Linux under QEMU, against build/libc/linux-*, for both formats.
set -u
echo "TEST-MARKER libm-long-double"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/libm-long-double
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/t.c" <<'SRC'
#include <math.h>
#include <float.h>
__attribute__((noinline)) static long double id(long double x) { return x; }
int main(void)
{
    long double huge = id(LDBL_MAX), sub = id(LDBL_MIN / 8), nz = id(-0.0L);
    if (isinf(huge) || !isfinite(huge)) return 1;        /* past double */
    if (fpclassify(sub) != FP_SUBNORMAL) return 2;       /* below double */
    if (fpclassify(huge) != FP_NORMAL || !isnormal(huge)) return 3;
    if (!signbit(nz) || signbit(fabsl(nz))) return 4;
    if (signbit(copysignl(nz, 1.0L))) return 5;
    if (!signbit(copysignl(1.0L, nz)) || copysignl(2.0L, -1.0L) != -2.0L)
        return 6;
    if (!isnan(id(NAN)) || !isinf(id(HUGE_VALL))) return 7;
    if (fpclassify(id(0.0L)) != FP_ZERO) return 8;
    return 42;
}
SRC

A64LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
A64GCC=$(dirname "$(aarch64-elf-gcc -print-libgcc-file-name 2>/dev/null)")

ran=0
for arch in x86_64 aarch64; do
    LIBDIR=$EMBCC_ROOT/build/libc/linux-$arch
    [ -f "$LIBDIR/libc.a" ] || { echo "skip $arch: no $LIBDIR/libc.a"; continue; }
    if [ $arch = aarch64 ] && { ! command -v "$A64LD" > /dev/null 2>&1 ||
                                [ ! -f "$A64GCC/libgcc.a" ]; }; then
        echo "skip $arch: no $A64LD and libgcc to link with"; continue
    fi
    "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch --check > /dev/null 2>&1 || {
        echo "skip $arch: no kernel for tests/harness/linux"; continue; }
    for O in -O0 -O2; do
        "$EMBCC" --target=$arch-linux-gnu $O -c "$out/t.c" -o "$out/t.o" \
            > "$out/cc.log" 2>&1 || {
            echo "FAIL $arch $O: did not compile"; cat "$out/cc.log"; exit 1; }
        if [ $arch = x86_64 ]; then
            "$EMBCC_ROOT/embld" -o "$out/t.bin" "$LIBDIR/crt1.o" "$out/t.o" \
                "$LIBDIR/libc.a" > "$out/ld.log" 2>&1
        else
            # embld writes no AArch64 executable; binary128 arithmetic
            # comes from libgcc, after libc.a (as tests/golden/linux.sh).
            "$A64LD" -static -T "$EMBCC_ROOT/lib/libc/os/linux/link.ld" \
                -o "$out/t.bin" "$LIBDIR/crt1.o" "$out/t.o" \
                "$LIBDIR/libc.a" -L"$A64GCC" -lgcc > "$out/ld.log" 2>&1
        fi || { echo "FAIL $arch $O: did not link"; cat "$out/ld.log"; exit 1; }
        "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch "$out/t.bin" \
            > "$out/run.log" 2>&1
        rc=$?
        [ "$rc" = 42 ] || {
            echo "FAIL $arch $O: exit $rc (42 expected)"; cat "$out/run.log"; exit 1; }
        ran=$((ran + 1))
    done
done
echo "libm-long-double: $ran runs, long double classified in its own format"
