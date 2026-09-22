#!/bin/sh
# The loop passes: LICM, rotation, and the value-numbering rule that
# rotation depends on.
#
# Each property is checked against the IR rather than the timing,
# because a benchmark says a loop got faster and not WHICH pass did it,
# and because a pass that silently stops firing is a regression no
# wall-clock test would fail on a fast machine.
#
# The third case is the important one. Value numbering builds its keys
# out of vreg numbers, which name a value only where the vreg is
# assigned once -- and mem2reg's phi destruction breaks exactly that for
# the vregs a loop revolves around. Before rotation there was one copy
# of a loop's test and nothing to collide with it; rotation makes a
# second copy on purpose, GCSE saw the guard dominating the latch, and
# replaced the rotated test with the guard's result. The loop's
# condition was then evaluated once, before the loop started. It
# terminated by luck of register allocation, which is the kind of bug
# that comes back, so the shape is pinned here.
set -eu
echo "TEST-MARKER loopopt"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/loopopt
rm -rf "$out"; mkdir -p "$out"

ir() {                          # ir SRC -> $out/ir.txt
    "$EMBCC" inspect ir --target=x86_64-linux-gnu -O2 -c "$1" -o /dev/null \
        > "$out/ir.txt" 2> "$out/err.txt" || {
        echo "FAIL: could not compile $1:"; cat "$out/err.txt"; exit 1; }
}

# ---- 1. LICM hoists, and hoists to the right side of the guard ----------
cat > "$out/licm.c" <<'EOF'
long f(long n, long a, long b)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += (a * b + a) ^ i;      /* a*b+a never changes */
    return s;
}
EOF
ir "$out/licm.c"
body=$(awk '/^L[0-9]*:/{blk=1} {print}' "$out/ir.txt")
# The multiply must not be inside the block the back edge returns to.
back=$(grep -E 'br(nz|z).* -> L[0-9]+' "$out/ir.txt" | tail -1 |
       sed 's/.*-> \(L[0-9]*\).*/\1/')
[ -n "$back" ] || { echo "FAIL: no loop branch in the IR"; exit 1; }
inloop=$(awk -v s="^$back:" '$0 ~ s {on=1; next} /^L[0-9]*:/{next} {if (on) print}' \
         "$out/ir.txt")
echo "$inloop" | grep -q 'mul' && {
    echo "FAIL: the invariant multiply is still inside the loop:"
    echo "$inloop"; exit 1; }
echo "LICM: the invariant multiply is out of the loop body"

# ---- 2. rotation: one conditional branch, no jump back ------------------
#
# A rotated loop ends with a conditional branch to its own top. An
# unrotated one ends with `jmp` to a header that branches out, which is
# two branches an iteration.
grep -q 'jmp L' "$out/ir.txt" && {
    echo "FAIL: the loop still jumps back to a top-tested header:"
    cat "$out/ir.txt"; exit 1; }
echo "rotation: the loop is bottom-tested -- one branch an iteration"

# ---- 3. the rotated test is RECOMPUTED, not value-numbered away --------
#
# Two `cmp` on the induction variable must survive: the guard's and the
# latch's. One means GCSE reused the guard's result, and the loop's
# condition is then a constant decided before it ran.
ncmp=$(grep -c 'cmp\.' "$out/ir.txt" || true)
[ "$ncmp" -ge 2 ] || {
    echo "FAIL: only $ncmp comparison(s) in a rotated loop. The guard and"
    echo "      the bottom test are different values of the induction"
    echo "      variable and must both be computed:"
    cat "$out/ir.txt"; exit 1; }
echo "value numbering: guard and bottom test are both computed ($ncmp compares)"

# ---- 4. strength reduction: the address is walked, not rebuilt ---------
#
# `a[i]` costs three instructions an iteration that have nothing to do
# with a[i]: widen i, shift by the element size, add the base. The value
# changes by a constant every iteration, so it can be walked by one add.
cat > "$out/sr.c" <<'EOF'
int a[4096];
int f(int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
EOF
"$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/sr.c" -o /dev/null \
    2> "$out/sr.txt" || { echo "FAIL: could not compile"; cat "$out/sr.txt"
                          exit 1; }
grep -q 'ivsr/address' "$out/sr.txt" || {
    echo "FAIL: the address is still recomputed from the index each"
    echo "      iteration:"; cat "$out/sr.txt"; exit 1; }
echo "strength reduction: the address is walked by one add"

# The pointer is walked at the END of the latch, after every phi copy.
# Next to `i += 1` looked natural and was wrong: the copies sit after
# the step, so `p = &a[i]` took the pointer AFTER it had moved on and
# came out as &a[i+1]. Checked by running it, below.

# ---- 5. and it all still runs ------------------------------------------
#
# The IR checks above say a transform fired; this says it was right. The
# answers come from gcc, and every loop here is one the passes rewrite:
# an invariant expression, a loop that runs zero times (so the guard has
# to hold), one that runs once, a nest, a loop whose bound changes inside
# it, and a loop carrying two values at once.
cat > "$out/run.c" <<'EOF'
#include <stdio.h>
static long invariant(long n, long a, long b)
{ long s = 0, i; for (i = 0; i < n; i++) s += (a * b + a) ^ i; return s; }

/* Zero trips: the rotated guard is the only thing standing between this
 * and one execution of the body. */
static long zero_trip(long n) { long s = 0, i; for (i = 0; i < n; i++) s += i * 3 + 1; return s; }

/* A nest, so the inner loop's preheader lands inside the outer body. */
static long nest(long n)
{
    long s = 0, i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            s += (i * 100) ^ j;         /* i*100 is invariant in j, not in i */
    return s;
}

/* The bound is written inside the loop, so nothing about it is
 * invariant -- and it only ever shrinks, so the loop still ends. */
static long moving_bound(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++) { s += i; if (s > 200 && n > i + 3) n = i + 3; }
    return s * 1000 + n;
}

/* Two loop-carried values, which is two phi temps to keep apart. */
static long two_carried(long n)
{
    long a = 1, b = 0, i;
    for (i = 0; i < n; i++) { long t = a + b; a = b; b = t; }
    return a * 31 + b;
}

/* A `while` with the test at the top and a `continue`, so the latch is
 * not the only back edge's source in the source text. */
static long with_continue(long n)
{
    long s = 0, i = 0;
    while (i < n) { i++; if (i % 3 == 0) continue; s += i * i; }
    return s;
}

/* An address taken FROM the walked pointer inside the loop and kept
 * after it. The copy happens among the phi copies in the latch, so it
 * catches a pointer that was incremented too early: this returned the
 * element one past the right one when the walk sat next to `i += 1`. */
static long addr_escapes(long n)
{
    long *p = 0, buf[64], i;
    for (i = 0; i < n && i < 64; i++) { p = &buf[i]; *p = i * 3; }
    return p ? *p * 1000 + (p - buf) : -1;
}

/* A loop whose body writes through a pointer: the load of *p may not be
 * hoisted, because the store can alias it. */
static long aliasing(long *p, long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++) { s += *p; p[i % 2] = s & 7; }
    return s;
}

int main(void)
{
    long buf[2]; buf[0] = 3; buf[1] = 5;
    unsigned long h = 0;
    h = h * 31 + (unsigned long)invariant(40, 7, 9);
    h = h * 31 + (unsigned long)invariant(0, 7, 9);      /* zero trips */
    h = h * 31 + (unsigned long)invariant(1, 7, 9);      /* one trip */
    h = h * 31 + (unsigned long)zero_trip(0);
    h = h * 31 + (unsigned long)zero_trip(-5);           /* negative bound */
    h = h * 31 + (unsigned long)zero_trip(17);
    h = h * 31 + (unsigned long)nest(9);
    h = h * 31 + (unsigned long)nest(0);
    h = h * 31 + (unsigned long)moving_bound(30);
    h = h * 31 + (unsigned long)two_carried(20);
    h = h * 31 + (unsigned long)with_continue(25);
    h = h * 31 + (unsigned long)aliasing(buf, 12);
    for (long k = 0; k <= 9; k++)
        h = h * 31 + (unsigned long)addr_escapes(k);
    h = h * 31 + (unsigned long)addr_escapes(40);
    printf("%lu\n", h);
    return 42;
}
EOF

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || { echo "skipped: no libc for linux-x86_64"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"; exit 0; }

got0= got1= got2=
for O in 0 1 2; do
    "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" -o "$out/r$O" \
        2> "$out/cc.log" || { echo "FAIL: build at -O$O:"; cat "$out/cc.log"
                              exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r$O" \
        > "$out/r$O.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: -O$O exited $rc:"; cat "$out/r$O.txt"
                        echo "      a loop that does not terminate here means"
                        echo "      the rotated test was value-numbered away"
                        exit 1; }
    ans=$(cat "$out/r$O.txt")
    case $O in 0) got0=$ans ;; 1) got1=$ans ;; 2) got2=$ans ;; esac
done
[ "$got0" = "$got1" ] && [ "$got1" = "$got2" ] || {
    echo "FAIL: the answer depends on the optimisation level --"
    echo "      -O0 $got0, -O1 $got1, -O2 $got2"; exit 1; }

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
if command -v "$GCC" > /dev/null 2>&1; then
    "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" -o "$out/g.o" \
        2> "$out/g.log" || { echo "FAIL: gcc will not build it:"
                             head -5 "$out/g.log"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
        "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
        "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" > "$out/g.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
    gans=$(cat "$out/g.txt")
    [ "$gans" = "$got2" ] || {
        echo "FAIL: gcc answers $gans, embcc -O2 answers $got2"; exit 1; }
    echo "seven loop shapes agree with gcc and across -O0/-O1/-O2:
invariant code, zero and one trip counts, a negative bound, a nest, a
bound written inside the loop, two carried values, a continue, and a
body that stores through a pointer it also loads"
else
    echo "seven loop shapes agree across -O0/-O1/-O2 (no $GCC to compare)"
fi
