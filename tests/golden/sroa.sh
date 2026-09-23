#!/bin/sh
# Splitting an aggregate into the scalars it is made of.
#
# A struct local is memory that mem2reg cannot touch: reading a field
# takes the object's address, and an address-taken variable is exactly
# what mem2reg refuses. So every field access is a real load, forever,
# however private the object is. SROA is the pass that notices the
# address never leaves `addr / add constant / load / store` and splits
# the object into one variable per field, which mem2reg then promotes.
#
# What can go wrong is not a missed optimization. Split an object whose
# pieces are not independent -- a union punned at two widths, an array
# indexed by a variable, an address that reached a callee -- and the
# program writes one thing and reads another. So the weight here is on
# the objects that must stay whole, and every one of them is checked by
# RUNNING the program against gcc rather than by reading the IR.
#
# Each object below is named uniquely, so the remark that decided it can
# be found without ambiguity.
set -eu
echo "TEST-MARKER sroa"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/sroa
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runner here is x86-64"; exit 0; }

cat > "$out/a.c" <<'EOF'
#include <stdio.h>
struct P { int x, y; };
struct N { struct P a; long k; };
union  U { double d; struct { int lo, hi; } w; };

int g_sink;
int *g_ptr;
/* noinline, or the inliner gets there first: a callee spliced into its
 * caller no longer HOLDS the address, and splitting would then be right
 * rather than wrong. These cases are about the call that stays a call. */
static __attribute__((noinline)) void take(struct P *p) { p->x += 1; }
static __attribute__((noinline)) int  fetch(void) { return g_sink; }

/* MUST split: a private record, read and written by field only */
static int must_plain(int a, int b)
{ struct P plain; plain.x = a; plain.y = b; return plain.x * 10 + plain.y; }

/* MUST split: a nested one, at a constant offset */
static long must_nested(int a, long k)
{ struct N nest; nest.a.x = a; nest.a.y = a + 1; nest.k = k;
  return nest.a.x + nest.a.y + nest.k; }

/* MUST split: an array indexed by constants only */
static int must_array(int a)
{ int arr[3]; arr[0] = a; arr[1] = a * 2; arr[2] = a * 3;
  return arr[0] + arr[1] + arr[2]; }

/* MUST split, but only after mem2reg: the address goes through a
 * pointer VARIABLE first, which is memory until the pointer is promoted */
static int must_via_pointer(int a)
{ struct P viaptr; int *q = &viaptr.x; *q = a; viaptr.y = a + 7;
  return *q * 2 + viaptr.y; }

/* MUST split: a scalar whose address is taken and never leaves */
static int must_scalar(int a)
{ int lone = a; int *r = &lone; *r = *r + 5; return lone; }

/* MUST NOT: the address reaches a callee, which may keep it */
static int keep_callee(int a)
{ struct P callee; callee.x = a; callee.y = a; take(&callee);
  return callee.x * 10 + callee.y; }

/* MUST NOT: punned at two widths -- the halves ARE the double */
static int keep_union(double d)
{ union U punned; punned.d = d; return punned.w.lo ^ punned.w.hi; }

/* MUST NOT: a variable index names any element */
static int keep_varindex(int a, int i)
{ int vary[4]; vary[0] = a; vary[1] = a+1; vary[2] = a+2; vary[3] = a+3;
  vary[i] = 99; return vary[0]+vary[1]+vary[2]+vary[3]; }

/* MUST NOT: the address escapes into a global */
static int keep_escapes(int a)
{ struct P leaks; leaks.x = a; leaks.y = a; g_ptr = &leaks.x; *g_ptr = 3;
  return leaks.x * 10 + leaks.y; }

/* MUST NOT: a volatile object's every access has to happen */
static int keep_volatile(int a)
{ volatile struct P vol; vol.x = a; vol.y = a + 1; return vol.x + vol.y; }

/* MUST NOT: observed by a call between the field writes */
static int keep_observed(int a)
{ struct P seen; seen.x = a; g_sink = seen.x; seen.y = fetch();
  take(&seen); return seen.x * 10 + seen.y; }

int main(void)
{
    unsigned long h = 0;
    for (int i = 1; i <= 9; i++) {
        g_sink = i;
        h = h*31 + (unsigned)must_plain(i, i*2);
        h = h*31 + (unsigned)must_nested(i, i*100);
        h = h*31 + (unsigned)must_array(i);
        h = h*31 + (unsigned)must_via_pointer(i);
        h = h*31 + (unsigned)must_scalar(i);
        h = h*31 + (unsigned)keep_callee(i);
        h = h*31 + (unsigned)keep_union((double)i + 0.5);
        h = h*31 + (unsigned)keep_varindex(i, i % 4);
        h = h*31 + (unsigned)keep_escapes(i);
        h = h*31 + (unsigned)keep_volatile(i);
        h = h*31 + (unsigned)keep_observed(i);
        h = h*31 + (unsigned)g_sink;
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
            echo "      An object was split whose pieces are not independent."
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
# The program above would pass with the pass doing nothing at all, so
# the decisions are asserted separately, from -fremarks rather than from
# reading assembly.
"$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/a.c" \
    -o /dev/null > "$out/rem.txt" 2>&1

split() { grep -q "split-into-scalars '$1'" "$out/rem.txt"; }
for v in plain nest arr viaptr lone; do
    split "$v" || {
        echo "FAIL: '$v' was not split, and nothing in its function stops it."
        grep "sroa/" "$out/rem.txt" | grep "'$v'" | sed 's/^/      /'
        exit 1; }
done
echo "the five private objects were split"

for v in callee punned vary leaks vol seen; do
    if split "$v"; then
        echo "FAIL: '$v' WAS split, and its pieces are not independent --"
        echo "      see the comment on its function in the case."
        exit 1
    fi
done
echo "the six that share, escape or must be observed stayed whole"

# Each guard that refuses one of them, named: a guard that stops firing
# would otherwise leave the case above passing for the wrong reason.
for r in two-accesses-overlap-at-different-widths address-escapes \
         declared-volatile; do
    grep -q "sroa/$r" "$out/rem.txt" || {
        echo "FAIL: nothing was refused for '$r', so that guard is not"
        echo "      exercised by any case here."
        grep "sroa/" "$out/rem.txt" | sed 's/^/      /'
        exit 1; }
done
echo "the union, the escaping address and the volatile object each named why"

# ---- 3. -fno-sroa turns it off -----------------------------------------
z=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fno-sroa -fremarks -c "$out/a.c" \
      -o /dev/null 2>&1 | grep -c "split-into-scalars" || true)
[ "$z" = 0 ] || { echo "FAIL: -fno-sroa still split $z objects"; exit 1; }
echo "-fno-sroa turns it off"

# ---- 4. and the frame shrinks ------------------------------------------
#
# The point of splitting is that the pieces go into registers, and the
# proof is the stack the function stops reserving: a slot no instruction
# names any more must not be laid out at all.
cat > "$out/f.c" <<'EOF'
struct P { int x, y; };
int only_fields(int a, int b)
{
    struct P p, q;
    p.x = a; p.y = b;
    q.x = b; q.y = a;
    return (p.x - q.x) * (p.y - q.y);
}
EOF
"$EMBCC" --target=x86_64-linux-gnu -O2 -S "$out/f.c" -o "$out/f.s" \
    2> "$out/cc.log" || { echo "FAIL: could not compile:"
                          cat "$out/cc.log"; exit 1; }
if grep -q 'push   %rbp' "$out/f.s"; then
    echo "FAIL: a function whose only locals are two split records still"
    echo "      builds a frame:"
    grep '#' "$out/f.s" | sed 's/.*#//' | sed 's/^/      /'
    exit 1
fi
echo "two split records leave a function with no frame at all"
