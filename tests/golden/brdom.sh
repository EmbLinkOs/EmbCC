#!/bin/sh
# A branch a dominating branch has already decided (pass_brdom in
# src/opt/cfgclean.c): `if (p) { ...; if (p) ... }` tests p once. And
# the trap the first draft fell into: the join of `if (!f || !(f->flags &
# W)) { if (f) ...; }` is reached from BOTH arms of the first test, so the
# inner `if (f)` is NOT decided -- the draft dropped the store on that
# path. Checked on three targets: the pass rewrites EmbIR.
set -u
echo "TEST-MARKER brdom"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/brdom
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/pos.c" << 'E'
void x(void); void y(void);
int h(int *p, int k)
{
    int r = 0;
    if (p) { x(); if (p) y(); r = 1; } else { r = 2; if (p) x(); }
    if (k > 3) { y(); if (k > 3) r += 4; }
    return r;
}
E
cat > "$out/neg.c" << 'E'
struct f { int flags; };
int g(struct f *f)
{
    if (!f || !(f->flags & 2)) { if (f) f->flags |= 8; return -1; }
    return f->flags;
}
E
for t in thumbv7em-none-eabi riscv32-unknown-elf x86_64-elf; do
    "$EMBCC" inspect ir -Os --target=$t "$out/pos.c" > "$out/pos-$t.ir" 2>&1 ||
        { cat "$out/pos-$t.ir"; fail "pos on $t: inspect"; }
    n=$(sed -n '/^func @h/,/^}/p' "$out/pos-$t.ir" | grep -c 'br[n]*z\.')
    [ "$n" = 2 ] || { sed -n '/^func @h/,/^}/p' "$out/pos-$t.ir"
                      fail "pos on $t: $n conditional branches, 2 decide everything"; }
    "$EMBCC" inspect ir -Os --target=$t "$out/neg.c" > "$out/neg-$t.ir" 2>&1 ||
        { cat "$out/neg-$t.ir"; fail "neg on $t: inspect"; }
    sed -n '/^func @g/,/^}/p' "$out/neg-$t.ir" | grep -q 'store' ||
        { sed -n '/^func @g/,/^}/p' "$out/neg-$t.ir"
          fail "neg on $t: the store behind the inner test is gone"; }
done
echo "brdom: a nested test of the same value is decided; a test at a join is not"

# and the values: the negative shape run, both ways
cat > "$out/run.c" << 'E'
struct f { int flags; };
__attribute__((noinline)) static int g(struct f *f)
{
    if (!f || !(f->flags & 2)) { if (f) f->flags |= 8; return -1; }
    return f->flags;
}
int main(void)
{
    struct f a = { 1 }, b = { 2 };
    int r = g(0) + g(&a) + g(&b);           /* -1 - 1 + 2 */
    return a.flags == 9 && b.flags == 2 && r == 0 ? 42 : 1;
}
E
"$EMBCC" --target=x86_64-elf -Os -c "$out/run.c" -o "$out/run.o" || fail "run.c: compile"
tests/harness/x86_64/link.sh -o "$out/run" "$out/run.o" > "$out/ln.log" 2>&1 || fail "run.c: link"
tests/harness/x86_64/run.sh "$out/run" > /dev/null 2>&1
rc=$?
[ "$rc" = 42 ] || fail "run.c exits $rc, not 42"
echo "brdom: the join's store runs, and the function returns what C says"
