#!/bin/sh
# EmbCC's output must not depend on which compiler built EmbCC.
#
# This is the property the self-host fixed point rests on, and nothing
# tested it. tests/golden/x86_64/self-host.sh proves that EmbCC compiles
# its own sources and that codegen is deterministic -- but determinism
# there means "the SAME binary, run twice", which by construction cannot
# catch a difference that depends on the HOST compiler.
#
# It could have. gen_float_to_u64 passed two IR-emitting calls as
# sibling arguments:
#
#     emit_bin(fn, IR_XOR, emit_f2i(...), emit_const(...), 8, 0)
#
# C leaves the order of sibling arguments unspecified and both append to
# the instruction list, so a gcc-built EmbCC and a clang-built one
# emitted instructions in different orders -- and the fixed point
# (stage1 building a byte-identical stage2) cannot exist when that is
# true. Two more of the same shape were in the 128-bit bitfield path.
#
# So: build EmbCC with two different host compilers, compile a corpus
# with both, and require every object to be byte-identical.
set -eu
echo "TEST-MARKER host-agnostic"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/host-agnostic
rm -rf "$out"; mkdir -p "$out"

# Two host compilers that are actually different. On macOS /usr/bin/gcc
# IS clang, so the names are not enough -- the version string decides,
# and where they turn out to be the same program this skips rather than
# comparing a compiler with itself and calling that agreement.
cc1=${EMBCC_HOST_CC1:-cc}
cc2=${EMBCC_HOST_CC2:-}
if [ -z "$cc2" ]; then
    for c in gcc clang cc; do
        command -v "$c" > /dev/null 2>&1 || continue
        [ "$("$c" --version 2>&1 | head -1)" = \
          "$("$cc1" --version 2>&1 | head -1)" ] && continue
        cc2=$c
        break
    done
fi
[ -n "$cc2" ] || {
    echo "skipped: only one host compiler here ($("$cc1" --version 2>&1 |
          head -1)) -- set EMBCC_HOST_CC2 to a different one"
    exit 0; }

echo "  host 1: $("$cc1" --version 2>&1 | head -1)"
echo "  host 2: $("$cc2" --version 2>&1 | head -1)"

# Do these two actually DISAGREE about argument evaluation order? If
# they do not, this comparison cannot see the class of bug it exists
# for, and saying "byte-identical" would be claiming a result the run
# did not earn. Both clangs on a Mac evaluate left to right; gcc on
# x86-64 does not, which is why the pair that matters is gcc + clang.
cat > "$out/ord.c" << 'EOF'
#include <stdio.h>
static int n;
static int tick(void) { return ++n; }
static int both(int a, int b) { return a * 10 + b; }
int main(void) { printf("%d\n", both(tick(), tick())); return 0; }
EOF
ord1=$("$cc1" -O2 -o "$out/ord1" "$out/ord.c" 2>/dev/null && "$out/ord1" \
       || echo "?")
ord2=$("$cc2" -O2 -o "$out/ord2" "$out/ord.c" 2>/dev/null && "$out/ord2" \
       || echo "?")
if [ "$ord1" = "$ord2" ]; then
    order_note="  note: both evaluate sibling arguments in the same order
  ($ord1), so this pair cannot expose an order-dependent emission; a
  gcc/clang pair can, and does on Linux"
else
    order_note="  these two DISAGREE about sibling argument order ($ord1
  vs $ord2), which is exactly the difference that made EmbCC's output
  depend on its host compiler"
fi

# Two builds, each into its own object directory so neither disturbs the
# tree's own build.
for n in 1 2; do
    eval "c=\$cc$n"
    ( cd "$EMBCC_ROOT" && make CC="$c" BUILD="$out/b$n" \
        "$out/embcc$n" ) > "$out/build$n.log" 2>&1 || {
        # The Makefile's embcc target writes ./embcc, so build it and
        # move it rather than fighting the rule.
        ( cd "$EMBCC_ROOT" && make CC="$c" BUILD="$out/b$n" embcc ) \
            > "$out/build$n.log" 2>&1 && cp "$EMBCC_ROOT/embcc" \
            "$out/embcc$n"; } || {
        echo "FAIL: could not build EmbCC with $c:"
        tail -15 "$out/build$n.log"; exit 1; }
    [ -f "$out/embcc$n" ] || cp "$EMBCC_ROOT/embcc" "$out/embcc$n"
done

# The corpus: every exec test, which is the widest body of C in the tree
# that compiles without arguments, plus the float conversions that are
# where the bug actually was.
cat > "$out/f2u.c" << 'EOF'
/* float -> unsigned 64, the path that had the unspecified order */
unsigned long long a(double d) { return (unsigned long long)d; }
unsigned long long b(float f)  { return (unsigned long long)f; }
unsigned long c(long double l) { return (unsigned long)l; }
struct w { __int128 x : 100; int y : 7; };
void s(struct w *p, __int128 v) { p->x = v; p->y = 3; }
__int128 g(struct w *p) { return p->x; }
EOF

differ=0
checked=0
for src in "$out/f2u.c" "$EMBCC_ROOT"/tests/exec/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    "$out/embcc1" -c "$src" -O2 -o "$out/$name.1.o" 2>/dev/null || continue
    "$out/embcc2" -c "$src" -O2 -o "$out/$name.2.o" 2>/dev/null || continue
    checked=$((checked + 1))
    cmp -s "$out/$name.1.o" "$out/$name.2.o" || {
        echo "FAIL: $name.c compiles DIFFERENTLY depending on which"
        echo "      compiler built EmbCC -- so no fixed point exists"
        differ=$((differ + 1))
    }
done
[ "$differ" = 0 ] || exit 1
[ "$checked" -gt 10 ] || {
    echo "FAIL: only $checked files compiled by both; the comparison"
    echo "      proves nothing at that size"; exit 1; }

echo "$checked objects are byte-identical whether EmbCC was built by
$cc1 or by $cc2"
echo "$order_note"
