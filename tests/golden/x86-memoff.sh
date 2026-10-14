#!/bin/sh
# x86-64: a field's constant offset folds into the access (ra_fold_memoff
# sets ir_ins.memoff; src/arch/x86_64/codegen.c reads it): `p->b` is
# `disp(base)`, not an add into a register and `(reg)`. Every store form
# must carry it -- the constant-store fusion once wrote `p[1] = 2` to
# (%rdi), and a struct of { 1, 2, 3, 4 } came out { 4, ?, ?, ? }; the
# base+index forms (`t->n[i] += t->pad[j]`) dropped it too. The
# program runs: it exits 42 only when every field holds what C says; and
# the disassembly shows the folded forms and no `add $4` before a store.
set -u
echo "TEST-MARKER x86-memoff"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/x86-memoff
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/m.c" <<'EOF'
struct s { int a, b, c, d; long e; short f; char g; };
__attribute__((noinline)) static void fill(struct s *p, int k)
{
    p->a = 1;                   /* constants: the fused const+store */
    p->b = 2;
    p->c = k;                   /* a register */
    p->d = p->b + k;            /* a load through the offset, then a store */
    p->e = 100000000000L;       /* an eight-byte constant */
    p->f = 7;
    p->g = 'x';
}
__attribute__((noinline)) static int sum(const struct s *p)
{
    return p->a + p->b + p->c + p->d + (int)(p->e / 100000000000L) + p->f + p->g;
}
__attribute__((noinline)) static int bump(struct s *p)
{
    p->b += 5;                  /* read-modify-write through the offset */
    p->c |= 8;
    return p->b == 7 && p->c == 11;
}
/* an array field indexed by a register: the address is base + index,
 * and the field's offset must still be in the access (base+index has no
 * displacement in the forms the backend fused it into) */
struct r { int pad[3]; unsigned n[4]; unsigned char st[4]; unsigned short h[4]; unsigned m[4]; };
__attribute__((noinline)) static void step(struct r *t, unsigned i)
{
    t->st[i] = (unsigned char)(t->st[i] + 3);   /* indexed byte load and store */
    t->h[i] = (unsigned short)(t->h[i] * 2 + 1);
    t->n[i] ^= 0x55u;
    t->n[i] += t->m[i];                         /* a load-op through base+index */
}
int main(void)
{
    struct r w = { { 10, 20, 30 }, { 1, 2, 3, 4 }, { 5, 6, 7, 8 },
                   { 9, 10, 11, 12 }, { 100, 200, 300, 400 } };
    for (unsigned i = 0; i < 4; i++)
        step(&w, i);
    for (unsigned i = 0; i < 4; i++)
        if (w.n[i] != ((i + 1) ^ 0x55u) + 100 * (i + 1) || w.st[i] != 8 + i ||
            w.h[i] != 2 * (9 + i) + 1 || w.m[i] != 100 * (i + 1) || w.pad[0] != 10)
            return 4;
    struct s v;
    fill(&v, 3);
    if (v.a != 1 || v.b != 2 || v.c != 3 || v.d != 5 || v.e != 100000000000L ||
        v.f != 7 || v.g != 'x')
        return 1;
    if (sum(&v) != 1 + 2 + 3 + 5 + 1 + 7 + 'x')
        return 2;
    if (!bump(&v))
        return 3;
    return 42;
}
EOF
for O in -O1 -Os -O2; do
    "$EMBCC" --target=x86_64-elf $O -c "$out/m.c" -o "$out/m.o" || fail "m.c $O: compile"
    tests/harness/x86_64/link.sh -o "$out/m" "$out/m.o" > "$out/ln.log" 2>&1 || fail "m.c $O: link"
    tests/harness/x86_64/run.sh "$out/m" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] || fail "m.c $O exits $rc, not 42: a field was stored at the wrong offset"
done
echo "x86-memoff: every field holds what C says at -O1, -Os and -O2"

command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP the disassembly half: no $OBJDUMP"; exit 0; }
"$EMBCC" --target=x86_64-elf -Os -c "$out/m.c" -o "$out/m.o" || fail "m.c: compile"
"$OBJDUMP" -d "$out/m.o" | sed -n '/<fill>:/,/^$/p' > "$out/fill.dis"
grep -q 'movl.*\$0x2, *0x4(%' "$out/fill.dis" ||
    { cat "$out/fill.dis"; fail "fill: p->b = 2 is not a store of 2 at 0x4(base)"; }
grep -q '0x10(%' "$out/fill.dis" ||
    { cat "$out/fill.dis"; fail "fill: p->e is not addressed at 0x10(base)"; }
n=$(grep -c 'add.*\$0x[0-9a-f]*, *%r' "$out/fill.dis")
[ "$n" = 0 ] || { cat "$out/fill.dis"; fail "fill: $n adds compute field addresses into registers"; }
echo "x86-memoff: fill addresses every field as disp(base), no add into a register"
