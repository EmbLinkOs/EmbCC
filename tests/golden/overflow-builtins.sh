#!/bin/sh
# __builtin_{add,sub,mul}_overflow in C, against the host compiler.
#
# The C++ front end has had these since it was written; C did not, and
# the GCC/Clang audit found it BY PROBE -- a grep for the name finds
# src/cxx/emit.c and says otherwise, which is how the first draft of
# that audit came to claim C had them. This test is the probe, kept.
#
# Everything here is decided at the boundaries, so that is all this
# checks: SMAX+1, SMIN-1, 0-1 unsigned, and the two multiplies that
# make the naive implementation trap rather than answer --
#
#   x * 0   the guard `a != 0 &&` exists because r / 0 traps
#   LMIN*-1 whose wrapped product is LMIN, and LMIN / -1 traps too --
#           the same case the test is trying to detect
#
# EmbCC's lowering folds both guards into the DIVISOR rather than
# branching on them (src/ir/irgen.c gen_overflow), so the sequence is
# branchless; these cases are what says the folding is right.
#
# The reference is the host compiler, run on the same source, because
# the answer is defined by the C abstract machine and not by anything
# in this tree. Both EmbCC front ends are compared against it: they
# reach the result by completely different routes -- C++ writes C text
# and C builds IR -- so agreeing is worth something.
set -u
echo "TEST-MARKER overflow-builtins"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/overflow-builtins
rm -rf "$out"; mkdir -p "$out"

cat > "$out/ov.c" <<'CEOF'
int printf(const char *, ...);

#define SMIN(T) ((T)((unsigned long long)1 << (sizeof(T)*8 - 1)))
#define SMAX(T) ((T)~SMIN(T))
#define UMAX(T) ((T)~(T)0)

#define CASE(tag, T, OP, A, B) do {                                   \
    T r = 0; int o = __builtin_##OP##_overflow((T)(A), (T)(B), &r);   \
    printf("%-22s %d %lld\n", tag, o, (long long)r);                  \
} while (0)

int main(void)
{
    CASE("sc add max+1",   signed char, add, SMAX(signed char), 1);
    CASE("sc add max+0",   signed char, add, SMAX(signed char), 0);
    CASE("sc sub min-1",   signed char, sub, SMIN(signed char), 1);
    CASE("sc mul 64*2",    signed char, mul, 64, 2);
    CASE("sc mul min*-1",  signed char, mul, SMIN(signed char), -1);
    CASE("uc add max+1",   unsigned char, add, UMAX(unsigned char), 1);
    CASE("uc sub 0-1",     unsigned char, sub, 0, 1);
    CASE("uc mul 16*16",   unsigned char, mul, 16, 16);
    CASE("i add max+1",    int, add, SMAX(int), 1);
    CASE("i add max+-1",   int, add, SMAX(int), -1);
    CASE("i sub min-1",    int, sub, SMIN(int), 1);
    CASE("i mul big",      int, mul, 65536, 65536);
    CASE("i mul min*-1",   int, mul, SMIN(int), -1);
    CASE("i mul 0*min",    int, mul, 0, SMIN(int));
    CASE("i mul -1*min",   int, mul, -1, SMIN(int));
    CASE("u add max+1",    unsigned, add, UMAX(unsigned), 1);
    CASE("u sub 0-1",      unsigned, sub, 0, 1);
    CASE("u mul big",      unsigned, mul, 65536, 65536);
    CASE("u mul 0*max",    unsigned, mul, 0, UMAX(unsigned));
    CASE("l add max+1",    long, add, SMAX(long), 1);
    CASE("l add min+-1",   long, add, SMIN(long), -1);
    CASE("l add max+0",    long, add, SMAX(long), 0);
    CASE("l sub min-1",    long, sub, SMIN(long), 1);
    CASE("l sub max--1",   long, sub, SMAX(long), -1);
    CASE("l sub ok",       long, sub, 100, 40);
    CASE("l mul min*-1",   long, mul, SMIN(long), -1);
    CASE("l mul -1*min",   long, mul, -1, SMIN(long));
    CASE("l mul 0*min",    long, mul, 0, SMIN(long));
    CASE("l mul min*0",    long, mul, SMIN(long), 0);
    CASE("l mul big",      long, mul, 4000000000L, 4000000000L);
    CASE("l mul ok",       long, mul, 123456, 654321);
    CASE("l mul -1*-1",    long, mul, -1, -1);
    CASE("l mul max*1",    long, mul, SMAX(long), 1);
    CASE("l mul max*2",    long, mul, SMAX(long), 2);
    CASE("ul add max+1",   unsigned long, add, UMAX(unsigned long), 1);
    CASE("ul add ok",      unsigned long, add, 5, 6);
    CASE("ul sub 0-1",     unsigned long, sub, 0, 1);
    CASE("ul sub ok",      unsigned long, sub, 9, 3);
    CASE("ul mul 0*max",   unsigned long, mul, 0, UMAX(unsigned long));
    CASE("ul mul max*0",   unsigned long, mul, UMAX(unsigned long), 0);
    CASE("ul mul max*2",   unsigned long, mul, UMAX(unsigned long), 2);
    CASE("ul mul big",     unsigned long, mul, 4000000000UL, 4000000000UL);
    CASE("ul mul ok",      unsigned long, mul, 1000, 1000);
    CASE("ul mul 1*max",   unsigned long, mul, 1, UMAX(unsigned long));
    return 0;
}
CEOF

# The reference: the host compiler on the same source.
HOSTCC=${CC:-cc}
"$HOSTCC" -O2 -w -o "$out/ref" "$out/ov.c" 2>/dev/null || {
    echo "skipped: the host compiler could not build the reference"; exit 0; }
"$out/ref" > "$out/want.txt" || { echo "FAIL: the reference did not run"; exit 1; }
n=$(wc -l < "$out/want.txt" | tr -d ' ')
[ "$n" -ge 40 ] || { echo "FAIL: the reference printed only $n lines"; exit 1; }

fail=0
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" "$opt" -c "$out/ov.c" -o "$out/ov.o" 2> "$out/e.txt" || {
        echo "FAIL $opt: does not compile"
        grep -oE 'error.*' "$out/e.txt" | head -1 | sed 's/^/     | /'
        fail=1; continue; }
    if t_link "$out/ov.bin" "$out/ov.o" 2>/dev/null &&
       t_run "$out/ov.bin" > "$out/got.txt" 2>/dev/null; then
        if cmp -s "$out/want.txt" "$out/got.txt"; then
            echo "  C $opt: all $n cases agree with the host"
        else
            echo "FAIL $opt: disagrees with the host"
            diff "$out/want.txt" "$out/got.txt" | head -8 | sed 's/^/     | /'
            fail=1
        fi
    else
        echo "  (skipped running $opt: no runner on this host)"
    fi
done

# The C++ front end reaches the same answers by a completely different
# route -- src/cxx/emit.c writes C text, src/ir/irgen.c builds IR -- so
# checking it here costs one compile and is worth it.
{ echo 'extern "C" int printf(const char *, ...);'
  sed '1d' "$out/ov.c"; } > "$out/ov.cc"
if "$EMBCC" -O2 -c "$out/ov.cc" -o "$out/ovxx.o" 2>/dev/null &&
   t_link "$out/ovxx.bin" "$out/ovxx.o" 2>/dev/null &&
   t_run "$out/ovxx.bin" > "$out/gotxx.txt" 2>/dev/null; then
    if cmp -s "$out/want.txt" "$out/gotxx.txt"; then
        echo "  C++ -O2: the other front end agrees with the host too"
    else
        echo "FAIL: the C and C++ front ends disagree"
        diff "$out/want.txt" "$out/gotxx.txt" | head -6 | sed 's/^/     | /'
        fail=1
    fi
fi

# A wrong shape must be refused by name, not miscompiled.
printf 'int f(int a,int b,float*r){return __builtin_add_overflow(a,b,r);}\n' \
    > "$out/bad.c"
if "$EMBCC" -fsyntax-only "$out/bad.c" > "$out/b.txt" 2>&1; then
    echo "FAIL: a float* result was accepted"; fail=1
else
    grep -q "points at the integer" "$out/b.txt" ||
        { echo "FAIL: refused, but not for the stated reason"
          head -1 "$out/b.txt"; fail=1; }
fi
printf 'int f(int a,int b){return __builtin_add_overflow(a,b);}\n' > "$out/bad2.c"
"$EMBCC" -fsyntax-only "$out/bad2.c" > /dev/null 2>&1 &&
    { echo "FAIL: two arguments were accepted"; fail=1; }
[ "$fail" -eq 0 ] && echo "  a float* result and a missing argument are refused by name"

[ "$fail" -eq 0 ] || exit 1
