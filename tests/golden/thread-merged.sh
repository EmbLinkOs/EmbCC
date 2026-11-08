#!/bin/sh
# Threading through a merged truth value or pointer (src/opt/cfgclean.c,
# pass_thread), in the two shapes an RTOS's scheduler and wait queues
# write and GCC threads:
#
#  1. `bool r = cur != idle && runnable(cur); if (r) ...`: the _Bool read
#     is `ext.4:1` of the `cmp ne` that tests the merged 0/1, and the
#     extension hid the test from the threading, so the 0/1 was built,
#     merged, extended and tested again (fold.c: an extension of a
#     comparison is a copy). Now no compare result is merged: every
#     branch is on the original conditions.
#  2. A list walk, `for (n = first(l); n && ...; n = next(l, n))`: the
#     NULL arm of `next` went through the loop's test of n, which it
#     already knew the answer to. The merged pointer has other readers,
#     so the arm keeps its definition and only its jump is threaded --
#     and that shape exists only after the joins' copies (opt.c), so
#     threading runs again there.
#  3. The scheduler's own: a store between the merge and its test
#     (`cpu.pending = 0` after `bool runnable = ...`). At -O2 the path is
#     copied after each arm that knows the answer (thread_paths).
#  4. One comparison two branches test, which value numbering shares:
#     issued again at the second (pass_cmpbranch), so each branch has a
#     comparison right before it to fuse with.
set -u
echo "TEST-MARKER thread-merged"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/thread-merged
rm -rf "$out"; mkdir -p "$out"
cat > "$out/b.c" <<'EOC'
typedef struct { unsigned char state, flags; int x; } th;
extern th idle;
static inline _Bool runnable_(const th *t) { return t->state != 2 && (t->flags & 0x13) == 1; }
void a(void); void b(void);
void f(th *cur)
{
    _Bool r = cur != &idle && runnable_(cur);
    if (r) a(); else b();
}
EOC
cat > "$out/w.c" <<'EOC'
struct n { struct n *next; int prio; };
struct l { struct n head; };
static inline struct n *first(struct l *l) { return l->head.next == &l->head ? 0 : l->head.next; }
static inline struct n *nxt(struct l *l, struct n *n) { return n->next == &l->head ? 0 : n->next; }
int g(struct l *l, int p)
{
    struct n *n;
    for (n = first(l); n && n->prio < p; n = nxt(l, n))
        ;
    return n ? n->prio : -1;
}
EOC
cat > "$out/s.c" <<'EOC'
typedef struct { unsigned char state, flags; int x; } th;
extern th idle;
struct cpu { th *cur; unsigned char pending; };
extern struct cpu cpu;
static inline _Bool runnable_(const th *t) { return t->state != 2 && (t->flags & 0x13) == 1; }
void a(void); void b(void); void c(void);
void s(void)
{
    th *cur = cpu.cur;
    _Bool r = cur != &idle && runnable_(cur);
    cpu.pending = 0;
    if (r) a(); else b();
}
void h(int *p, int *q, int k)
{
    if (p != q) a();
    if (k) c();
    if (p != q) b();
}
EOC
fail=0
"$EMBCC" inspect ir "$out/s.c" --target=riscv32-none-elf -O2 > "$out/s.ir" || exit 1
sed -n '/func @s /,/^}/p' "$out/s.ir" > "$out/fs.ir"
# the three conditions, compared once each, and no merged 0/1 re-tested
nc=$(grep -cE '= cmp\.' "$out/fs.ir"); nx=$(grep -cE ' ne %[0-9]+, #0' "$out/fs.ir")
[ "$nc" = 3 ] && [ "$nx" = 0 ] || {
    echo "FAIL: s() has $nc comparisons (want 3), $nx re-tests (want 0):"; cat "$out/fs.ir"; fail=1; }
sed -n '/func @h /,/^}/p' "$out/s.ir" > "$out/fh.ir"
# the two branches on p != q test two comparisons, not one shared
dup=$(grep -E 'br(n)?z' "$out/fh.ir" | grep -v ' %2 ' | awk '{print $2}' | sort | uniq -d)
[ -z "$dup" ] && [ "$(grep -cE '= cmp[.]' "$out/fh.ir")" -ge 2 ] || {
    echo "FAIL: h()'s second test of p != q shares the first's comparison:"; cat "$out/fh.ir"; fail=1; }
for o in -O2 -Os; do
    "$EMBCC" inspect ir "$out/b.c" --target=riscv32-none-elf $o > "$out/b$o.ir" || exit 1
    # every branch on a comparison, none on a merged or extended 0/1
    nb=$(grep -cE '^ +br(n)?z' "$out/b$o.ir")
    nx=$(grep -cE '= ext\.|cmp\.[0-9]+s? ne %[0-9]+, #0' "$out/b$o.ir")
    [ "$nb" = 3 ] && [ "$nx" = 0 ] || {
        echo "FAIL $o: f() has $nb branches (want 3) and $nx re-tests of a 0/1 (want 0):"
        sed -n '/func @f/,/^}/p' "$out/b$o.ir"; fail=1; }
    "$EMBCC" inspect ir "$out/w.c" --target=riscv32-none-elf $o > "$out/w$o.ir" || exit 1
    # the NULL arm jumps to the -1, not to the loop test that decides it
    sed -n '/func @g/,/^}/p' "$out/w$o.ir" > "$out/g$o.ir"
    lm=$(grep -E '= const\.4s? -1' -B1 "$out/g$o.ir" | sed -n 's/^\(L[0-9]*\):.*/\1/p' | head -1)
    # more ways into the -1 than the final test's: a NULL arm goes there
    [ -n "$lm" ] && [ "$(grep -cE "(-> |jmp )$lm([[:space:]]|\$)" "$out/g$o.ir")" -ge 2 ] || {
        echo "FAIL $o: g()'s NULL arm does not go straight to the -1:"; cat "$out/g$o.ir"; fail=1; }
done
[ "$fail" = 0 ] || exit 1
echo "thread-merged: a _Bool of an && is never built and re-tested, a list walk's NULL arm skips the test it decides, a store between merge and test is copied (-O2), and a shared comparison is issued at each branch"
