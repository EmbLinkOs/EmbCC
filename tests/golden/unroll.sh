#!/bin/sh
# Copies of a loop body, with one test between them.
#
# A short loop spends more on being a loop than on its work: `s += i & 7`
# is three instructions of arithmetic behind an increment, a compare and
# a branch. Running the body four times per test spends those once
# instead of four times.
#
# The trip count is not known, so the copies cannot simply replace the
# loop -- there has to be somewhere for the leftover iterations to go.
# There is: the ORIGINAL loop, kept exactly as it was, with the copies
# inserted in front of it and falling into it when fewer than U remain.
# That is what most of this test is about, because getting the handover
# wrong does not produce a slower program, it produces one that runs the
# body a different number of times. So every case below is checked by
# RUNNING it, at every optimization level and against gcc, over trip
# counts either side of the unroll factor -- including zero, one, and
# exactly U.
set -eu
echo "TEST-MARKER unroll"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/unroll
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runner here is x86-64"; exit 0; }

cat > "$out/a.c" <<'EOF'
#include <stdio.h>
int ga[80], gb[80];
static __attribute__((noinline)) int side(int x) { return x ^ 0x5a; }

/* the shapes that unroll */
static long sum_and(long n)   { long s=0,i; for (i=0;i<n;i++) s += i & 7; return s; }
static long sum_arr(int n)    { long s=0; for (int i=0;i<n;i++) s += ga[i]; return s; }
static long store_arr(int n)  { for (int i=0;i<n;i++) gb[i] = ga[i]*3+1;
                                return gb[n>0?n-1:0]; }
static long ptrwalk(int n)    { long s=0; int *p=ga; for (int i=0;i<n;i++) s += *p++;
                                return s; }
/* the counter is live after the loop: the last copy has to leave it
 * where the loop would have */
static long liveout(int n)    { int i; long s=0; for (i=0;i<n;i++) s+=i;
                                return s*100+i; }
/* a value carried through two variables at once */
static long twocarry(int n)   { long a=1,b=0; for (int i=0;i<n;i++) { long t=a+b; b=a; a=t; }
                                return a*1000+b; }
/* the INNER loop of a nest is a counted loop like any other */
static long nested(int n)     { long s=0; for (int i=0;i<n;i++)
                                    for (int j=0;j<n;j++) s += i*j; return s; }

/* the shapes that must not, each for its own reason */
static long calls(int n)      { long s=0; for (int i=0;i<n;i++) s += side(i); return s; }
static long step2(int n)      { long s=0; for (int i=0;i<n;i+=2) s+=i; return s; }
static long down(int n)       { long s=0; for (int i=n;i>0;i--) s+=i; return s; }
static long brk(int n)        { long s=0; for (int i=0;i<n;i++) { if (i==7) break; s+=i; }
                                return s; }
static long cont(int n)       { long s=0; for (int i=0;i<n;i++) { if (i&1) continue; s+=i; }
                                return s; }

int main(void)
{
    unsigned long h = 0;
    for (int k = 0; k < 80; k++) { ga[k] = k*7-13; gb[k] = 0; }
    /* -1 through 40 covers every remainder of every unroll factor this
     * pass uses, and the counts where the loop runs zero or one time. */
    for (int n = -1; n <= 40; n++) {
        int m = n < 0 ? 0 : (n > 80 ? 80 : n);
        h = h*1000003u + (unsigned long)sum_and(n);
        h = h*1000003u + (unsigned long)sum_arr(m);
        h = h*1000003u + (unsigned long)store_arr(m);
        h = h*1000003u + (unsigned long)ptrwalk(m);
        h = h*1000003u + (unsigned long)liveout(n);
        h = h*1000003u + (unsigned long)twocarry(n);
        h = h*1000003u + (unsigned long)calls(n);
        h = h*1000003u + (unsigned long)step2(n);
        h = h*1000003u + (unsigned long)down(n);
        h = h*1000003u + (unsigned long)brk(n);
        h = h*1000003u + (unsigned long)cont(n);
        h = h*1000003u + (unsigned long)nested(n > 8 ? 8 : n);
    }
    printf("%lu\n", h);
    return 42;
}
EOF

# ---- 1. the same answer at every level, and the same as gcc's ----------
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    prev=
    for O in 0 1 2; do
        "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/a.c" -o "$out/r" \
            2> "$out/cc.log" || { echo "FAIL: build at -O$O:"
                                  cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: exited $rc at -O$O:"
                            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: answers $ans at -O$O where a lower level said $prev."
            echo "      A loop ran a different number of times once unrolled."
            exit 1; }
        prev=$ans
    done
    echo "the same answer at -O0, -O1 and -O2 ($prev)"

    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/a.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/g.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the case"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" \
            > "$out/g.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
        [ "$(cat "$out/g.txt")" = "$prev" ] || {
            echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
            exit 1; }
        echo "and the same answer as gcc"
    fi
else
    echo "(not run: needs a kernel and a libc for tests/harness/linux)"
fi

# ---- 2. what it decided --------------------------------------------------
#
# The program above passes with the pass doing nothing, so which loops it
# took is asserted separately, from -fremarks.
"$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/a.c" \
    -o /dev/null > "$out/rem.txt" 2>&1
took() { grep -q "unrolled '$1'" "$out/rem.txt"; }

for f in sum_and sum_arr store_arr ptrwalk liveout twocarry nested; do
    took "$f" || {
        echo "FAIL: '$f' was not unrolled, and it is the shape this pass"
        echo "      exists for -- a counted loop stepping by one."
        grep unroll "$out/rem.txt" | sed 's/^/      /'
        exit 1; }
done
echo "the seven counted loops were unrolled, the nest's inner one included"

# Each of these is refused for a stated reason, and a reason that stopped
# working would leave the running test above passing for the wrong cause.
#   calls  -- a call dwarfs the loop overhead, and costs U copies of it
#   step2  -- the step is not one, so `n - i` is not the iterations left
#   down   -- counts down; the test is not `iv < invariant`
#   brk/cont -- the body is not one straight line
for f in calls step2 down brk cont; do
    if took "$f"; then
        echo "FAIL: '$f' WAS unrolled. See the comment above this loop in"
        echo "      the test for why that is not a shape this pass handles."
        exit 1
    fi
done
echo "the five it cannot or should not take were left alone"

# ---- 3. -fno-unroll turns it off, and -Os never turns it on ------------
z=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fno-unroll -fremarks -c "$out/a.c" \
      -o /dev/null 2>&1 | grep -c "unrolled '" || true)
[ "$z" = 0 ] || { echo "FAIL: -fno-unroll still unrolled $z loops"; exit 1; }
z=$("$EMBCC" --target=x86_64-linux-gnu -Os -fremarks -c "$out/a.c" \
      -o /dev/null 2>&1 | grep -c "unrolled '" || true)
[ "$z" = 0 ] || { echo "FAIL: -Os unrolled $z loops, and unrolling is the
one transformation here that always adds code"; exit 1; }
echo "-fno-unroll turns it off, and -Os never turns it on"

# ---- 4. and the remainder loop really is the original ------------------
#
# The handover is the whole risk, so it gets its own case: a loop whose
# trip count is one short of the unroll factor every time must still run
# the body exactly that many times.
cat > "$out/r.c" <<'EOF'
#include <stdio.h>
static int seen;
static long body(long n) { long s = 0; for (long i = 0; i < n; i++) { seen++; s += i; }
                           return s; }
int main(void)
{
    for (long n = 0; n <= 33; n++) {
        seen = 0;
        long s = body(n);
        long want = n * (n - 1) / 2;
        if (seen != n || s != want) {
            printf("n=%ld ran %d times summing %ld, wanted %ld times and %ld\n",
                   n, seen, s, n, want);
            return 1;
        }
    }
    printf("every trip count from 0 to 33 ran exactly that many times\n");
    return 42;
}
EOF
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    "$EMBCC" --target=x86_64-linux-gnu -O2 "$out/r.c" -o "$out/rr" \
        2> "$out/cc.log" || { echo "FAIL: could not build the trip-count case:"
                              cat "$out/cc.log"; exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/rr" > "$out/rr.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the trip-count case exited $rc:"
                        cat "$out/rr.txt"; exit 1; }
    cat "$out/rr.txt"
fi
