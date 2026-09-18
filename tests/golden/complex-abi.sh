#!/bin/sh
# _Complex ACROSS the compiler boundary: EmbCC and gcc halves calling each
# other both ways, for float, double and long double _Complex. EmbCC passes
# and returns a complex exactly as struct { T re, im; } — on x86-64 that is
# SSE-class for float/double (one packed xmm, or two), and for long double
# memory in and st0/st1 out; on aarch64 an HFA in v registers — so a
# mismatch here reads garbage, and only a cross check can see it. The EmbCC
# half also uses newlib's <complex.h> (its `I` is the GNU `1.0fi`) and calls
# newlib's libm on complex values.
set -u
echo "TEST-MARKER complex-abi"
. "$(dirname "$0")/../lib.sh"
out_dir="tests/golden/out/complex-abi-$ARCH"
rm -rf "$out_dir"; mkdir -p "$out_dir"

cat > "$out_dir/x.h" << 'EOF'
typedef float _Complex fc;
typedef double _Complex dc;
typedef long double _Complex lc;
fc gcc_f(fc a, fc b);                 /* a * b + 1 */
dc gcc_d(dc a, double k, dc b);       /* a * k - b */
lc gcc_l(lc a, lc b);                 /* a * b */
dc gcc_many(dc a, dc b, dc c, dc d, dc e, dc f);   /* past the registers */
double gcc_vsum(int n, ...);          /* n double _Complex: sum of re + im */
fc emb_f(fc a, fc b);
dc emb_d(dc a, double k, dc b);
lc emb_l(lc a, lc b);
int gcc_call_emb(void);
EOF

cat > "$out_dir/gcchalf.c" << 'EOF'
#include <stdarg.h>
#include "x.h"
fc gcc_f(fc a, fc b) { return a * b + 1; }
dc gcc_d(dc a, double k, dc b) { return a * k - b; }
lc gcc_l(lc a, lc b) { return a * b; }
dc gcc_many(dc a, dc b, dc c, dc d, dc e, dc f) { return a + b + c + d + e + f; }
double gcc_vsum(int n, ...)
{
    va_list ap; double s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) { dc z = va_arg(ap, dc); s += __real__ z + __imag__ z; }
    va_end(ap);
    return s;
}
int gcc_call_emb(void)
{
    fc f = emb_f(1.0f + 2.0fi, 3.0f - 1.0fi);            /* (5 + 5i) + 1 */
    if (__real__ f != 6 || __imag__ f != 5) return 0;
    dc d = emb_d(2.0 + 1.0i, 3.0, 1.0 - 1.0i);            /* 5 + 4i */
    if (__real__ d != 5 || __imag__ d != 4) return 0;
    lc l = emb_l(1.0L + 2.0Li, 3.0L - 1.0Li);             /* 5 + 5i */
    if (__real__ l != 5 || __imag__ l != 5) return 0;
    return 1;
}
EOF

cat > "$out_dir/embhalf.c" << 'EOF'
#include <complex.h>
#include "x.h"
fc emb_f(fc a, fc b) { return a * b + 1; }
dc emb_d(dc a, double k, dc b) { return a * k - b; }
lc emb_l(lc a, lc b) { return a * b; }
int main(void)
{
    /* EmbCC -> gcc */
    fc f = gcc_f(1.0f + 2.0f * I, 3.0f - I);
    if (crealf(f) != 6 || cimagf(f) != 5) return 1;
    dc d = gcc_d(2.0 + I, 3.0, 1.0 - I);
    if (creal(d) != 5 || cimag(d) != 4) return 2;
    lc l = gcc_l(1.0L + 2.0L * I, 3.0L - I);
    if (__real__ l != 5 || __imag__ l != 5) return 3;
    dc m = gcc_many(1, I, 2, 2 * I, 3, 3 * I);
    if (m != 6 + 6 * I) return 4;
    if (gcc_vsum(3, 1.0 + 2.0 * I, 3.0 - I, 0.5 * I) != 5.5) return 5;
    /* gcc -> EmbCC */
    if (!gcc_call_emb()) return 6;
    /* newlib's libm on complex values */
    if (cabs(3.0 + 4.0 * I) != 5.0) return 7;
    if (conj(1.0 + 2.0 * I) != 1.0 - 2.0 * I) return 8;
    return 42;
}
EOF

t_gcc_c "$out_dir/gcchalf.c" -std=c11 -I "$out_dir" -o "$out_dir/gcchalf.o" || {
    echo "gcc half failed to build"; exit 1; }
[ "$ARCH" = x86_64 ] && inc="-I $X86_NEWLIB/include" || inc="-I $AARCH64_NEWLIB/include"
# shellcheck disable=SC2086
"$EMBCC" --target="$TARGET" -c "$out_dir/embhalf.c" -o "$out_dir/embhalf.o" \
    -I "$out_dir" -I include $inc || { echo "embcc half failed to build"; exit 1; }
t_link "$out_dir/prog" "$out_dir/embhalf.o" "$out_dir/gcchalf.o" || {
    echo "link failed"; exit 1; }
t_run "$out_dir/prog" >/dev/null; got=$?
[ "$got" -eq 42 ] || {
    echo "cross run exited $got (a nonzero N is the Nth check)"; exit 1; }
echo "EmbCC and gcc agree on float/double/long double _Complex across the"
echo "boundary ($ARCH), and EmbCC builds against newlib's <complex.h> and libm"
