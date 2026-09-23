#!/bin/sh
# Alias analysis, and the two passes that depend on it.
#
# "Can these two memory references be the same bytes?" is the question
# every pass that moves, reuses or deletes a memory operation has to
# ask, and getting it wrong does not look like a missing optimization --
# it looks like a program that writes a value and then reads a
# different one.
#
# So the weight here is on the references that DO alias. Each case below
# is one the analysis must refuse to separate: the same global twice,
# two pointers that turn out to be equal, a local whose address escaped,
# a callee that can reach what the caller wrote, and anything volatile.
# They are checked by running the program against gcc, not by reading
# the IR, because the question is what the program computes.
set -eu
echo "TEST-MARKER alias"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/alias
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runner here is x86-64"; exit 0; }
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || { echo "skipped: no libc for linux-x86_64"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"; exit 0; }

# ---- 1. what must be separated, and what must not ----------------------
cat > "$out/a.c" <<'EOF'
#include <stdio.h>
int g1[32], g2[32];
struct S { int a, b; };
static int sink;
static void wr(int *p) { *p = 99; }

/* MUST see the write: the same global read either side of a store to it */
static int same_global(int i) { int x = g1[i]; g1[i] = 7; return x + g1[i]; }
/* MUST see it: two pointers the caller happens to make equal */
static int two_ptrs(int *p, int *q) { int x = *p; *q = 5; return x * 10 + *p; }
/* MUST see it: a local whose address escaped into a pointer */
static int escaped(void) { int l = 1; int *p = &l; int x = l; *p = 42; return x * 100 + l; }
/* MUST see it: a callee holding the address */
static int thru_call(void) { int l = 3; int x = l; wr(&l); return x * 100 + l; }
/* MUST see it: one field written between two reads of another is fine,
 * but a write to the SAME field is not */
static int fields(struct S *s)
{ int x = s->a; s->b = 11; int y = s->a; s->a = 22; return x*100 + y*10 + s->a; }

/* MUST NOT be confused: distinct globals, and a local that never escapes
 * sitting beside a store through an unknown pointer. */
static int distinct(int i, int *q) { int x = g1[i]; g2[i] = 8; *q = 9; return x + g1[i]; }
static int private_local(int *q) { int l = 4; int x = l; *q = 77; return x * 10 + l; }

int main(void)
{
    unsigned long h = 0;
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 32; k++) { g1[k] = k + 1; g2[k] = -k; }
        h = h*31 + (unsigned)same_global(i);
        h = h*31 + (unsigned)two_ptrs(&g1[i], &g1[i]);   /* the same object */
        h = h*31 + (unsigned)two_ptrs(&g1[i], &g2[i]);   /* different ones */
        h = h*31 + (unsigned)escaped();
        h = h*31 + (unsigned)thru_call();
        { struct S s = { i, i*2 }; h = h*31 + (unsigned)fields(&s); }
        h = h*31 + (unsigned)distinct(i, &sink);
        h = h*31 + (unsigned)private_local(&sink);
        h = h*31 + (unsigned)sink;
    }
    printf("%lu\n", h);
    return 42;
}
EOF

# ---- 2. dead stores: the one that is, and the four that are not --------
cat > "$out/d.c" <<'EOF'
#include <stdio.h>
struct S { int a, b, c; };
int g[16];
static int seen;
static void obs(void);
static void dead(struct S *p)      { p->a = 1; p->a = 2; }        /* dead */
static int  read_between(struct S *p) { p->a = 1; int x = p->a; p->a = 2; return x; }
static void call_between(struct S *p) { p->a = 1; obs(); p->a = 2; }
static void other_field(struct S *p)  { p->a = 1; p->b = 2; }
static void volatile_store(volatile int *v) { *v = 1; *v = 2; }
static int  dead_local(int n)      { int x = 1; x = 2; return x + n; }
static void obs(void) { seen = g[0]; }
int main(void)
{
    unsigned long h = 0; struct S s; volatile int v = 0;
    for (int i = 0; i < 6; i++) {
        s.a = i; s.b = i*2; s.c = i*3; g[0] = i;
        dead(&s);            h = h*31 + (unsigned)(s.a + s.b + s.c);
        s.a = i;             h = h*31 + (unsigned)read_between(&s) + (unsigned)s.a;
        s.a = i; call_between(&s); h = h*31 + (unsigned)(s.a + seen);
        s.a = i; s.b = i; other_field(&s); h = h*31 + (unsigned)(s.a*10 + s.b);
        h = h*31 + (unsigned)dead_local(i);
        volatile_store(&v);  h = h*31 + (unsigned)v;
    }
    printf("%lu\n", h);
    return 42;
}
EOF

run3() {                        # run3 SRC -> the answer, same at every level
    prev=
    for O in 0 1 2; do
        "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$1" -o "$out/r" \
            2> "$out/cc.log" || { echo "FAIL: build at -O$O:"
                                  cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: $1 exited $rc at -O$O:"
                            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: $1 answers $ans at -O$O where a lower level said $prev."
            echo "      A reference was separated from one it aliases."
            exit 1; }
        prev=$ans
    done
    ANSWER=$prev
}

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
check_gcc() {                   # check_gcc SRC EXPECTED
    command -v "$GCC" > /dev/null 2>&1 || return 0
    "$GCC" -O2 -c "$1" -I"$EMBCC_ROOT/lib/libc/include" -o "$out/g.o" \
        2> /dev/null || { echo "FAIL: gcc will not build $1"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
        "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
        "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" > "$out/g.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
    [ "$(cat "$out/g.txt")" = "$2" ] || {
        echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $2"; exit 1; }
}

run3 "$out/a.c"; check_gcc "$out/a.c" "$ANSWER"
echo "aliasing references stay together: the same global, two pointers
that are equal, an escaped local, a callee holding the address, and the
same struct field -- while distinct globals and a private local do not"

run3 "$out/d.c"; check_gcc "$out/d.c" "$ANSWER"
echo "dead stores agree with gcc at every level"

# ---- 3. and the passes actually fire -----------------------------------
#
# The programs above would still pass if both passes did nothing, so the
# counts are asserted separately. They come from -fremarks rather than
# from reading assembly.
n=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/d.c" \
      -o /dev/null 2>&1 | sed -n 's/.*, \([0-9]*\) dead stores.*/\1/p' |
    awk '{s+=$1} END {print s+0}')
[ "$n" -ge 1 ] || { echo "FAIL: no dead store was removed, so the test
above is not measuring anything"; exit 1; }
echo "dead store elimination removed $n"

cat > "$out/l.c" <<'EOF'
int g1[64], g2[64];
int f(int i, int n)
{
    int s = 0;
    /* the store to g2 must not evict the cached load from g1 */
    for (int k = 0; k < n; k++) { s += g1[i]; g2[k] = s; s += g1[i]; }
    return s;
}
EOF
l=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/l.c" \
      -o /dev/null 2>&1 | sed -n 's/.*, \([0-9]*\) load reuse.*/\1/p' |
    awk '{s+=$1} END {print s+0}')
[ "$l" -ge 1 ] || { echo "FAIL: a load from g1 was not reused across a
store to g2, so the alias analysis is not being consulted"; exit 1; }
echo "a load survives a store to a different global ($l reused)"

# ---- 4. -fno- turns them off -------------------------------------------
z=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fno-dse -fremarks -c "$out/d.c" \
      -o /dev/null 2>&1 | sed -n 's/.*, \([0-9]*\) dead stores.*/\1/p' |
    awk '{s+=$1} END {print s+0}')
[ "$z" = 0 ] || { echo "FAIL: -fno-dse still removed $z stores"; exit 1; }
echo "-fno-dse turns it off"
