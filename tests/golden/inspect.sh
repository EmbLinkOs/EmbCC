#!/bin/sh
# `embcc inspect` — the stages, in text (vision §18, ARCHITECTURE §2).
#
# The IR is the boundary between language semantics and machine code, and
# until this existed nothing could look at it: a pass that went wrong was
# debugged by reading the machine code it eventually produced. The test that
# matters is not that the printer emits *something* — it is that the same
# command at two -O levels SHOWS THE OPTIMIZER'S WORK, because that is the
# thing the dump was built to make visible.
set -eu
echo "TEST-MARKER inspect"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/inspect
rm -rf "$out"; mkdir -p "$out"

cat > "$out/p.c" << 'EOF'
extern int g;
int add(int a, int b) { return a + b; }
int sum(int n)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        t = add(t, i);
    g = t;
    return t;
}
EOF

# ---- the shape of the form ------------------------------------------------
"$EMBCC" inspect ir "$out/p.c" -O0 > "$out/O0.txt"
head -12 "$out/O0.txt"
grep -q "^; EmbIR" "$out/O0.txt" || { echo "FAIL: no header"; exit 1; }
grep -q "^func @add {" "$out/O0.txt" || { echo "FAIL: add missing"; exit 1; }
grep -q "^func @sum {" "$out/O0.txt" || { echo "FAIL: sum missing"; exit 1; }
# Widths read left to right: result, then memory, then flags.
grep -qE "ldvar\.[48]:[1248]s? v[0-9]+" "$out/O0.txt" ||
    { echo "FAIL: ldvar suffix"; exit 1; }
# Every instruction that came from source carries its line (R3, provenance).
grep -q "; line 2" "$out/O0.txt" || { echo "FAIL: no line provenance"; exit 1; }
echo "the form: a header, a func per function, widths as .result:memory+flags,
and a source line on every instruction that has one"

# ---- what it is FOR: the optimizer becomes visible -------------------------
"$EMBCC" inspect ir "$out/p.c" -O2 > "$out/O2.txt"

# At -O0 the loop variables live in stack slots and are reloaded every turn.
[ "$(grep -c 'ldvar' "$out/O0.txt")" -ge 6 ] ||
    { echo "FAIL: expected stack traffic at -O0"; exit 1; }
o0=$(grep -c 'ldvar' "$out/O0.txt")
o2=$(grep -c 'ldvar' "$out/O2.txt")
[ "$o2" -lt "$o0" ] ||
    { echo "FAIL: -O2 did not reduce stack traffic ($o0 -> $o2)"; exit 1; }
echo "mem2reg is visible: ldvar $o0 -> $o2"

# add() is inlined at -O2, and the giveaway is provenance: an instruction
# inside sum() carrying add()'s line. That is R3 surviving a transformation,
# which is exactly what a line table needs and what no other test checks.
awk '/^func @sum/,/^}/' "$out/O2.txt" > "$out/sum2.txt"
grep -q "; line 2" "$out/sum2.txt" ||
    { cat "$out/sum2.txt"; echo "FAIL: add() was not inlined into sum(), or"
      echo "      the inliner dropped the callee's source line"; exit 1; }
grep -q "call @add" "$out/sum2.txt" &&
    echo "note: the call survives too (partial inlining)"
echo "inlining is visible: sum() holds an instruction from add()'s line 2,
so provenance survived the transformation"

# The immediate-fold pass turns `i + 1` into an operand.
grep -qE "add\.[48]s? %[0-9]+, #1" "$out/O2.txt" ||
    { echo "FAIL: immediate fold not visible"; exit 1; }
echo "immediate folding is visible: an operand printed as #1"

# ---- the other stage, and the error path ----------------------------------
"$EMBCC" inspect pp "$out/p.c" > "$out/pp.txt"
grep -q "int sum(int n)" "$out/pp.txt" || { echo "FAIL: pp"; exit 1; }
echo "inspect pp: the preprocessed source"

# ---- the front-end stages -------------------------------------------------
cat > "$out/s.c" << 'EOF'
struct Packet {
    unsigned char kind;
    unsigned int flags : 3;
    unsigned int prio  : 5;
    int          len;
    char        *data;
};
static int total;
int process(struct Packet *p) { total += p->len; return p->flags; }
EOF

"$EMBCC" inspect tokens "$out/s.c" > "$out/tok.txt"
grep -q "^   1:1   'struct'" "$out/tok.txt" || { echo "FAIL: tokens"; exit 1; }
grep -q "'Packet'  *Packet" "$out/tok.txt" || { echo "FAIL: ident text"; exit 1; }
grep -qE "^; [0-9]+ tokens" "$out/tok.txt" || { echo "FAIL: count"; exit 1; }
echo "inspect tokens: $(sed -n 's/^; \([0-9]*\) tokens/\1/p' "$out/tok.txt"),
each with its line and column"

# The AST carries RESOLVED types -- it runs after sema, because a tree with
# every type shown as "?" answers a question nobody asks.
"$EMBCC" inspect ast "$out/s.c" > "$out/ast.txt"
sed "s|$out/||" "$out/ast.txt" | head -8
grep -q "^function process : int(struct Packet \* p)" "$out/ast.txt" ||
    { echo "FAIL: ast signature"; exit 1; }
grep -q "member ->len : int" "$out/ast.txt" || { echo "FAIL: ast member"; exit 1; }
grep -q "var p : struct Packet \*" "$out/ast.txt" ||
    { echo "FAIL: ast resolved type"; exit 1; }
echo "inspect ast: the tree, with the types sema resolved"

"$EMBCC" inspect symbols "$out/s.c" > "$out/sym.txt"
grep -q "^function  process .*int(struct Packet \*)" "$out/sym.txt" ||
    { cat "$out/sym.txt"; echo "FAIL: symbols"; exit 1; }
grep -q "^variable  total .*static" "$out/sym.txt" ||
    { echo "FAIL: static not shown"; exit 1; }
echo "inspect symbols: what the unit declares, with linkage"

# ---- layout: the decision the compiler never explains ----------------------
"$EMBCC" inspect types "$out/s.c" > "$out/ty.txt"
cat "$out/ty.txt"
grep -q "^struct Packet  (16 bytes, align 8)" "$out/ty.txt" ||
    { echo "FAIL: struct size"; exit 1; }
# A bit-field's position is the part nobody can predict from the source.
grep -q "flags .*: 3  (bits 8-10 of the unit at 0)" "$out/ty.txt" ||
    { echo "FAIL: bit-field position"; exit 1; }
grep -q "prio .*: 5  (bits 11-15 of the unit at 0)" "$out/ty.txt" ||
    { echo "FAIL: second bit-field"; exit 1; }
# Padding is invisible in the source and expensive in RAM, so it is named.
grep -q "(padding) *2 bytes" "$out/ty.txt" ||
    { echo "FAIL: padding not reported, or reported at the wrong size"; exit 1; }
echo "inspect types: offsets, bit-field positions, and the padding between"

# And the layout is the REAL one: the referee agrees on the numbers.
if command -v "${EMBCC_REFEREE_GCC:-x86_64-elf-gcc}" > /dev/null 2>&1; then
    cat > "$out/off.c" << 'EOF'
#include <stddef.h>
struct Packet {
    unsigned char kind;
    unsigned int flags : 3;
    unsigned int prio  : 5;
    int          len;
    char        *data;
};
char a[sizeof(struct Packet) == 16 ? 1 : -1];
char b[offsetof(struct Packet, len) == 4 ? 1 : -1];
char c[offsetof(struct Packet, data) == 8 ? 1 : -1];
EOF
    "${EMBCC_REFEREE_GCC:-x86_64-elf-gcc}" -c -isystem "$X86_NEWLIB/include"         "$out/off.c" -o "$out/off.o" 2> "$out/off.err" ||
        { cat "$out/off.err"
          echo "FAIL: gcc disagrees with the layout we printed"; exit 1; }
    echo "and gcc agrees: 16 bytes, len at 4, data at 8"
fi

# Flags still mean what they mean: inspect replaces a compilation, it does
# not change one.
"$EMBCC" inspect ir "$out/p.c" -O0 -DUNUSED=1 > /dev/null ||
    { echo "FAIL: ordinary flags rejected"; exit 1; }
[ ! -f "$out/p.o" ] || { echo "FAIL: inspect wrote an object"; exit 1; }
echo "ordinary flags work, and no object is written"

if "$EMBCC" inspect nosuch "$out/p.c" > "$out/err.txt" 2>&1; then
    echo "FAIL: unknown stage accepted"; exit 1
fi
grep -q "unknown stage" "$out/err.txt" || { echo "FAIL: no message"; exit 1; }
echo "an unknown stage is refused by name"
