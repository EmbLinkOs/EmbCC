#!/bin/sh
# A leaf that needs no frame should not build one.
#
# `push rbp; mov rsp,rbp; sub $N,rsp; ...; leave` is four instructions a
# two-line function does not need, and it used to pay them in full --
# opaque_add(a,b) was nine instructions where gcc emits two, with a
# sixteen-byte frame holding nothing, because a local got a stack slot
# even after the register allocator had put it in a register.
#
# Two properties, and the second is why the first is safe:
#
#  1. A small leaf emits no frame at all.
#  2. A function that DOES need one still gets it. Each case here is a
#     way rbp is still reachable -- a call (the stack has to be aligned
#     and the frame walked), an address that escapes, alloca, varargs,
#     more arguments than fit in registers, and -g (DWARF describes a
#     local as an offset from the frame base).
#
# The last of those is the one that bit: a stack-passed parameter reaches
# its allocated register BY WAY of its home slot, so eliding the slot
# made a 14-argument call return the wrong answer. It is checked by
# running it, not by reading the code.
set -eu
echo "TEST-MARKER frameless"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/frameless
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: the frameless prologue is x86-64 so far"; exit 0; }

# has_frame SRC FLAGS -> 0 when the object contains `push %rbp`
has_frame() {
    "$EMBCC" --target=x86_64-linux-gnu -O2 $2 -S "$1" -o "$out/t.s" \
        2> "$out/cc.log" || { echo "FAIL: could not compile:"
                              cat "$out/cc.log"; exit 1; }
    grep -q 'push   %rbp' "$out/t.s"
}

# ---- 1. a leaf with nothing in its frame -------------------------------
cat > "$out/leaf.c" <<'EOF'
long add(long a, long b) { return a + b; }
int  pick(int a, int b, int c) { return a > b ? a + c : b - c; }
EOF
if has_frame "$out/leaf.c" ""; then
    echo "FAIL: a leaf that touches no stack still builds a frame:"
    grep '#' "$out/t.s" | sed 's/.*#//'
    exit 1
fi
echo "a leaf with no frame builds none"

# ---- 2. and everything that still needs one ----------------------------
for case in call addr alloca varargs manyargs; do
    case $case in
    call)     cat > "$out/n.c" <<'EOF'
long g(long);
long f(long a) { return g(a) + 1; }
EOF
              why="it calls, so the stack must stay aligned" ;;
    addr)     cat > "$out/n.c" <<'EOF'
void g(long *);
long f(long a) { long x = a + 1; g(&x); return x; }
EOF
              why="a local's address escapes" ;;
    alloca)   cat > "$out/n.c" <<'EOF'
void g(char *);
long f(long n) { char *p = __builtin_alloca((unsigned long)n); g(p); return p[0]; }
EOF
              why="alloca moves the stack pointer" ;;
    varargs)  cat > "$out/n.c" <<'EOF'
#include <stdarg.h>
long f(int n, ...) { va_list ap; va_start(ap, n); long v = va_arg(ap, long);
                     va_end(ap); return v; }
EOF
              why="a variadic prologue spills the argument file" ;;
    manyargs) cat > "$out/n.c" <<'EOF'
long f(long a,long b,long c,long d,long e,long f2,long g,long h)
{ return a+b+c+d+e+f2+g+h; }
EOF
              why="arguments arrive on the caller's stack" ;;
    esac
    has_frame "$out/n.c" "" || {
        echo "FAIL: '$case' lost its frame, but $why"
        exit 1; }
done
echo "a call, an escaping address, alloca, varargs and stack arguments
each still build one"

# ---- 3. -g keeps it, because DWARF describes locals from the frame base -
if ! has_frame "$out/leaf.c" "-g"; then
    echo "FAIL: -g dropped the frame pointer, so DW_OP_fbreg has no base"
    exit 1
fi
echo "-g keeps the frame pointer"

# ---- 4. the answers are unchanged --------------------------------------
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || { echo "skipped the run: no libc"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped the run: no kernel for tests/harness/linux"; exit 0; }

cat > "$out/run.c" <<'EOF'
#include <stdio.h>
static long add(long a, long b) { return a + b; }
static int  pick(int a, int b, int c) { return a > b ? a + c : b - c; }
/* Past six integer registers: these arrive on the stack and still have
 * to reach their allocated registers. */
static long many(long a,long b,long c,long d,long e,long f,long g,long h,
                 long i,long j,long k,long l,long m,long n)
{ return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10+k*11+l*12+m*13+n*14; }
static long escapes(long a) { long x = a * 3; long *p = &x; *p += 1; return x; }
int main(void)
{
    unsigned long h = 0;
    for (int q = 0; q < 16; q++) {
        h = h * 31 + (unsigned long)add(q, q * 7);
        h = h * 31 + (unsigned)pick(q, 8, q + 1);
        h = h * 31 + (unsigned long)many(q,q+1,q+2,q+3,q+4,q+5,q+6,
                                         q+7,q+8,q+9,q+10,q+11,q+12,q+13);
        h = h * 31 + (unsigned long)escapes(q);
    }
    printf("%lu\n", h);
    return 42;
}
EOF
prev=
for O in 0 1 2; do
    "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" -o "$out/r" \
        2> "$out/cc.log" || { echo "FAIL: build at -O$O:"; cat "$out/cc.log"
                              exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" > "$out/r.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: -O$O exited $rc:"; cat "$out/r.txt"; exit 1; }
    ans=$(cat "$out/r.txt")
    [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
        echo "FAIL: -O$O answers $ans where a lower level answered $prev"
        echo "      a parameter lost on its way to its register reads as"
        echo "      whatever the register happened to hold"
        exit 1; }
    prev=$ans
done

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
if command -v "$GCC" > /dev/null 2>&1; then
    "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" -o "$out/g.o" \
        2> /dev/null || { echo "FAIL: gcc will not build it"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
        "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
        "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" > "$out/g.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
    [ "$(cat "$out/g.txt")" = "$prev" ] || {
        echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
        exit 1; }
    echo "a leaf, a fourteen-argument call and an escaping local agree with
gcc and across -O0/-O1/-O2"
else
    echo "the answers agree across -O0/-O1/-O2 (no $GCC to compare)"
fi
