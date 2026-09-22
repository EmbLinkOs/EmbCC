#!/bin/sh
# Sibling calls: that the stack stops growing, and that nothing else does.
#
# `return f(args);` need not build a frame on top of this one. Tearing
# this frame down BEFORE the call and jumping rather than calling leaves
# the callee returning straight to our caller -- so a tail-recursive
# function runs in constant stack instead of eventually overflowing.
#
# The property is not "faster", it is "does not run out". So the test is
# a recursion deep enough that the ordinary lowering CANNOT survive it,
# run at both -O1 (where sibling calls are off) and -O2 (where they are
# on). -O1 must die and -O2 must produce the answer; a test where both
# pass would be measuring nothing.
#
# And the correctness half: every guard in tail_call_ok exists because
# some frame is still referenced after the jump, so each one is a
# program here that would break if the transform ignored it.
set -eu
echo "TEST-MARKER tailcall"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/tailcall
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: sibling calls are x86-64 so far (the aarch64 backend
still builds a frame for every call)"; exit 0; }
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || { echo "skipped: no libc for linux-x86_64"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"
    exit 0; }

build() {                       # build SRC OUT -O<n>
    "$EMBCC" --target=x86_64-linux-gnu "$3" "$1" -o "$2" 2> "$out/cc.log" || {
        echo "FAIL: did not build $1 at $3:"; cat "$out/cc.log"; exit 1; }
}
run() {                         # run BIN -> rc, output in $out/run.txt
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$1" > "$out/run.txt" 2>&1
    rc=$?
    set -e
}

# ---- 1. the stack stops growing -----------------------------------------
cat > "$out/deep.c" << 'EOF'
#include <stdio.h>
/* Three million frames: far past any stack a process is given, so the
 * ordinary lowering cannot reach the bottom. */
static long sum_to(long n, long acc) { return n == 0 ? acc : sum_to(n - 1, acc + n); }
int main(void)
{
    long r = sum_to(3000000L, 0);
    printf("%ld\n", r);
    return r == 4500001500000L ? 42 : 1;
}
EOF
build "$out/deep.c" "$out/deep2" -O2
run "$out/deep2"
[ "$rc" = 42 ] || {
    echo "FAIL: three million tail calls did not survive -O2 (exit $rc):"
    cat "$out/run.txt"
    echo "      the frame is supposed to be replaced rather than stacked"
    exit 1; }
deep_answer=$(cat "$out/run.txt")

build "$out/deep.c" "$out/deep1" -O1
run "$out/deep1"
[ "$rc" != 42 ] || {
    echo "FAIL: -O1 survived three million frames too, so this test is"
    echo "      not measuring the transform -- either sibling calls came"
    echo "      on at -O1 or the recursion is no longer deep enough"
    exit 1; }
echo "3000000 frames deep: -O2 returns $deep_answer, -O1 runs out of stack"

# ---- 2. and the guards ---------------------------------------------------
#
# Each of these is a frame that is still referenced after the jump would
# have happened. They are in one program so one wrong answer is one
# failure, and the expected values come from gcc.
cat > "$out/guards.c" << 'EOF'
#include <stdio.h>

/* A local's ADDRESS handed to the callee. After a tail jump this frame
 * is gone and the pointer is into dead stack, so the transform must not
 * fire here. */
static int by_addr(int *p) { return *p + 1; }
static int takes_addr(int v) { int local = v * 2; return by_addr(&local); }

/* Stack arguments: the outgoing area overlaps the frame being torn
 * down, so the seventh argument onwards would be written into it. */
static int nine(int a,int b,int c,int d,int e,int f,int g,int h,int i)
{ return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9; }
static int many(int v) { return nine(v,v+1,v+2,v+3,v+4,v+5,v+6,v+7,v+8); }

/* A variadic caller keeps its register save area in the frame. */
static int vsum(int n, ...) { return n * 3; }
static int from_variadic(int n, ...) { return vsum(n, 1, 2); }

/* A struct return travels through a hidden pointer into the CALLER's
 * buffer, which is a second thing to get right. */
struct P { long a, b, c; };
static struct P make(long v) { struct P p; p.a=v; p.b=v*2; p.c=v*3; return p; }
static struct P forward(long v) { return make(v); }

/* Mutual tail recursion, which is the case a single-function check
 * would miss. */
static int odd(int n);
static int even(int n) { return n == 0 ? 1 : odd(n - 1); }
static int odd(int n)  { return n == 0 ? 0 : even(n - 1); }

/* Tail calls returning each shape of value. */
static double dtail(double x) { return x * 2.0; }
static double dcall(double x) { return dtail(x + 1.0); }
static long double ltail(long double x) { return x * 3.0L; }
static long double lcall(long double x) { return ltail(x + 1.0L); }
static void vtail(int *p) { *p += 5; }
static void vcall(int *p) { vtail(p); }

int main(void)
{
    unsigned long h = 0;
    int i, sink = 0;
    for (i = 0; i < 20; i++) {
        h = h * 31 + (unsigned)takes_addr(i);
        h = h * 31 + (unsigned)many(i);
        h = h * 31 + (unsigned)from_variadic(i, 7, 8);
        { struct P p = forward(i); h = h * 31 + (unsigned)(p.a + p.b + p.c); }
        h = h * 31 + (unsigned)even(i) + (unsigned)odd(i);
        h = h * 31 + (unsigned)(long)(dcall((double)i) * 4);
        h = h * 31 + (unsigned)(long)(lcall((long double)i) * 4);
        sink = i; vcall(&sink); h = h * 31 + (unsigned)sink;
    }
    printf("%lu\n", h);
    return 42;
}
EOF
got0= got1= got2=
for O in 0 1 2; do
    build "$out/guards.c" "$out/guards$O" "-O$O"
    run "$out/guards$O"
    [ "$rc" = 42 ] || { echo "FAIL: the guard program exited $rc at -O$O:"
                        cat "$out/run.txt"; exit 1; }
    ans=$(cat "$out/run.txt")
    case $O in 0) got0=$ans ;; 1) got1=$ans ;; 2) got2=$ans ;; esac
done
[ "$got0" = "$got1" ] && [ "$got1" = "$got2" ] || {
    echo "FAIL: the answer depends on the optimisation level --"
    echo "      -O0 $got0, -O1 $got1, -O2 $got2."
    echo "      A sibling call fired where the frame was still needed"
    exit 1; }

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
if command -v "$GCC" > /dev/null 2>&1; then
    LIBGCC=$("$GCC" -print-libgcc-file-name)
    "$GCC" -O2 -c "$out/guards.c" -I"$EMBCC_ROOT/lib/libc/include" \
        -o "$out/g.o" 2> "$out/g.log" || {
        echo "FAIL: gcc will not build the guard program:"
        head -5 "$out/g.log"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
        "$LIBDIR/libc.a" "$LIBDIR/librt.a" "$LIBGCC" 2>/dev/null
    run "$out/g"
    [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
    gccans=$(cat "$out/run.txt")
    [ "$gccans" = "$got2" ] || {
        echo "FAIL: gcc answers $gccans, embcc -O2 answers $got2"; exit 1; }
    echo "the guards hold at every level and agree with gcc: a local's
address escaping, stack arguments, a variadic caller, a struct return,
mutual recursion, and tail calls returning double, long double and void"
else
    echo "the guards hold at every level (no $GCC to compare against)"
fi
