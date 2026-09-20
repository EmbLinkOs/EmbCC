#!/bin/sh
# Provenance: R3, and the verifier that keeps it (vision §9.1, §14).
#
# "Every AST node, IR instruction, machine instruction and emitted byte
# carries a source location... A transformation that drops provenance is a
# bug, and the IR verifier checks for it."
#
# The check is the point. A pass that builds a replacement instruction from
# scratch, instead of copying the one it replaces, drops the location — and
# nothing downstream complains, because a line table with a hole still links.
# Only the verifier can notice.
set -eu
echo "TEST-MARKER provenance"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/provenance
rm -rf "$out"; mkdir -p "$out"

# ---- the IR carries a column, not just a line -----------------------------
cat > "$out/col.c" << 'EOF'
extern int a[10], b[10];
int f(int i, int j) { return a[i] + b[j]; }
EOF
"$EMBCC" inspect ir "$out/col.c" -O0 > "$out/ir.txt"
sed -n '4,12p' "$out/ir.txt"

# Two subscripts on one line must not share a column: that is the whole
# reason a line alone is not enough.
ca=$(grep "gaddr @a" "$out/ir.txt" | sed 's/.*; 2:\([0-9]*\).*/\1/')
cb=$(grep "gaddr @b" "$out/ir.txt" | sed 's/.*; 2:\([0-9]*\).*/\1/')
[ -n "$ca" ] && [ -n "$cb" ] || { echo "FAIL: no columns in the IR"; exit 1; }
[ "$ca" -lt "$cb" ] ||
    { echo "FAIL: a[i] ($ca) should come before b[j] ($cb)"; exit 1; }
echo "the IR carries columns: a[i] at 2:$ca, b[j] at 2:$cb -- one line, two places"

# ---- the verifier rejects an instruction with no location -----------------
# Run over EmbCC's own source at -O2, where mem2reg, SCCP, the inliner and
# the out-of-SSA rebuild all run. Every one of them builds instructions from
# scratch, and every one of them used to drop the location.
holes=0
checked=0
for f in src/*/*.c src/*/*/*.c; do
    checked=$((checked + 1))
    if EMBCC_VERIFY=1 "$EMBCC" -c -O2 -Isrc -isystem "$X86_NEWLIB/include" \
           "$f" -o "$out/v.o" 2>&1 | grep -q "no source location"; then
        holes=$((holes + 1))
        echo "  hole in $f"
    fi
done
[ "$holes" = 0 ] ||
    { echo "FAIL: $holes of $checked files have an instruction with no location"
      exit 1; }
echo "$checked of EmbCC's own sources at -O2: every instruction has a location
or is marked compiler-synthesized"

# The exception §9.1 allows is marked, not merely absent -- otherwise the
# verifier cannot tell a deliberate case from a pass that forgot.
cat > "$out/sy.c" << 'EOF'
extern int g(int);
int f(int n) { int t = 0; for (int i = 0; i < n; i++) t += g(i); return t; }
EOF
"$EMBCC" inspect ir "$out/sy.c" -O2 > "$out/sy.txt"
if grep -q "compiler-synthesized" "$out/sy.txt"; then
    echo "and the synthesized ones say so:
  $(grep -m1 "compiler-synthesized" "$out/sy.txt" | sed 's/^ *//')"
fi

# ---- C++ expressions carry a column too -----------------------------------
# Before this, every C++ error reported column 0, which renders as a line
# with no caret at all.
cat > "$out/e.cc" << 'EOF'
struct A { void f(); };
int main() { A a; int x = a.f() + 1; return x; }
EOF
"$EMBCC" -c "$out/e.cc" -o "$out/e.o" > "$out/e.txt" 2>&1 || true
sed "s|$out/||" "$out/e.txt"
grep -qE "e\.cc:2:[0-9]+: error:" "$out/e.txt" ||
    { echo "FAIL: a C++ error still has no column"; exit 1; }
col=$(sed -n 's/.*e\.cc:2:\([0-9]*\): error.*/\1/p' "$out/e.txt" | head -1)
[ "$col" -gt 0 ] || { echo "FAIL: column 0"; exit 1; }
# The caret is what the column buys: a line without one is what this fixed.
grep -q "\\^" "$out/e.txt" || { echo "FAIL: no caret under the error"; exit 1; }
echo "a C++ error points at column $col, with a caret -- it reported 0 before"
