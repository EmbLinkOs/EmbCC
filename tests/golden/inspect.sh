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
grep -qE "^func @add .*\{" "$out/O0.txt" || { echo "FAIL: add missing"; exit 1; }
grep -qE "^func @sum .*\{" "$out/O0.txt" || { echo "FAIL: sum missing"; exit 1; }
# Widths read left to right: result, then memory, then flags.
grep -qE "ldvar\.[48]:[1248]s? v[0-9]+" "$out/O0.txt" ||
    { echo "FAIL: ldvar suffix"; exit 1; }
# Every instruction that came from source carries its line AND column
# (R3) -- `; 2:30`, since provenance.sh made the column real.
grep -qE "; 2:[0-9]+" "$out/O0.txt" || { echo "FAIL: no line:col provenance"; exit 1; }
# The header carries the function's shape, which is what lets the text be
# read back (tests/golden/ir-roundtrip.sh).
grep -qE "^func @sum .*nparams=1 nvars=[0-9]+ vregs=" "$out/O0.txt" ||
    { echo "FAIL: the func header lost its shape"; exit 1; }
grep -qE "^  local v0 size=4 align=4" "$out/O0.txt" ||
    { echo "FAIL: the frame slots are not described"; exit 1; }
echo "the form: a header carrying each function's shape, its frame slots with
their sizes and alignments, widths as .result:memory+flags, and a source
line and column on every instruction that has one"

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
grep -qE "; 2:[0-9]+" "$out/sum2.txt" ||
    { cat "$out/sum2.txt"; echo "FAIL: add() was not inlined into sum(), or"
      echo "      the inliner dropped the callee's source line"; exit 1; }
grep -q "call @add" "$out/sum2.txt" &&
    echo "note: the call survives too (partial inlining)"
echo "inlining is visible: sum() holds an instruction from add()'s line 2,
so provenance survived the transformation -- including the parameter stores
the inliner invents, which the verifier now insists carry the call's place"

# The immediate-fold pass turns `i + 1` into an operand.
grep -qE "add\.[48]s? %[0-9]+, #1" "$out/O2.txt" ||
    { echo "FAIL: immediate fold not visible"; exit 1; }
echo "immediate folding is visible: an operand printed as #1"

# ---- the other stage, and the error path ----------------------------------
"$EMBCC" inspect pp "$out/p.c" > "$out/pp.txt"
grep -q "int sum(int n)" "$out/pp.txt" || { echo "FAIL: pp"; exit 1; }
echo "inspect pp: the preprocessed source"

# ---- the control-flow graph -----------------------------------------------
# It comes from the SAME builder the passes use (R1), so what it prints is
# the graph mem2reg and CSE reason about, not a second one written for the
# dump -- which would disagree exactly where somebody was debugging.
cat > "$out/cg.c" << 'EOF'
extern int leaf(int);
extern void (*hook)(void);
static int helper(int x) { return leaf(x) + 1; }
int outer(int n)
{
    int t = 0;
    for (int i = 0; i < n; i++) { if (i & 1) continue; t += helper(i); }
    hook();
    return t;
}
EOF
"$EMBCC" inspect cfg "$out/cg.c" -O0 > "$out/cfg.txt"
sed -n '/function outer/,/^$/p' "$out/cfg.txt" | head -12
grep -qE "^function outer: [0-9]+ blocks" "$out/cfg.txt" ||
    { echo "FAIL: no block count"; exit 1; }
grep -qE "^  B[0-9]+ +ins \[[0-9]+,[0-9]+\)" "$out/cfg.txt" ||
    { echo "FAIL: no block ranges"; exit 1; }
grep -q "       from B" "$out/cfg.txt" || { echo "FAIL: no predecessors"; exit 1; }
grep -q "       idom B" "$out/cfg.txt" || { echo "FAIL: no dominators"; exit 1; }
# A loop is a back edge -- a successor that dominates you -- and naming it is
# the difference between a block list and a control-flow graph.
grep -q "back edge to B.* (a loop)" "$out/cfg.txt" ||
    { cat "$out/cfg.txt"; echo "FAIL: the loop's back edge is not identified"; exit 1; }
echo "inspect cfg: blocks, edges, dominators, and the back edge that makes
the for-loop a loop"

# ---- the call graph -------------------------------------------------------
"$EMBCC" inspect callgraph "$out/cg.c" -O0 > "$out/cg0.txt"
grep -q "^helper (static)" "$out/cg0.txt" || { echo "FAIL: linkage"; exit 1; }
grep -q -- "-> leaf   (not defined in this unit)" "$out/cg0.txt" ||
    { cat "$out/cg0.txt"; echo "FAIL: external callee"; exit 1; }
grep -q -- "-> (through a function pointer)" "$out/cg0.txt" ||
    { echo "FAIL: indirect call"; exit 1; }
grep -q -- "-> helper" "$out/cg0.txt" || { echo "FAIL: internal call"; exit 1; }

# It is read from the IR, so at -O2 it is the graph AFTER inlining -- which
# is the graph the linker and a stack analysis will actually see.
"$EMBCC" inspect callgraph "$out/cg.c" -O2 > "$out/cg2.txt"
if grep -q -- "-> helper" "$out/cg2.txt"; then
    cat "$out/cg2.txt"; echo "FAIL: -O2 should have inlined helper away"; exit 1
fi
grep -q -- "-> leaf" "$out/cg2.txt" ||
    { echo "FAIL: outer should call leaf directly once helper is inlined"; exit 1; }
echo "inspect callgraph: at -O0 outer calls helper; at -O2 helper is inlined
and outer calls leaf directly -- the graph the linker will see"

# ---- a stage the design names but the compiler does not have --------------
if "$EMBCC" inspect mir "$out/cg.c" > "$out/mir.txt" 2>&1; then
    echo "FAIL: inspect mir claimed to work"; exit 1
fi
grep -q "no EmbMIR" "$out/mir.txt" ||
    { cat "$out/mir.txt"; echo "FAIL: should say why, not 'unknown stage'"; exit 1; }
grep -q "inspect ir" "$out/mir.txt" ||
    { echo "FAIL: should point at the nearest view"; exit 1; }
echo "inspect mir says there is no EmbMIR and what to look at instead"

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
