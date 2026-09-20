#!/bin/sh
# Remarks, and `embcc why` over them (vision R2 / ADR-0004, §13, §19).
#
# "Every pass that makes a decision emits a structured remark AT THAT POINT.
# 'Why?' features are queries over recorded remarks — never a separate engine
# reconstructing reasons after the fact."
#
# The test that matters is not that a remark prints. It is that DIFFERENT
# causes produce DIFFERENT reasons: the inliner used to return 0 from a dozen
# places and every one of them meant something else, and by the time anyone
# asked, the dozen answers had collapsed into one.
set -eu
echo "TEST-MARKER remarks"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/remarks
rm -rf "$out"; mkdir -p "$out"

cat > "$out/p.c" << 'EOF'
#include <stdarg.h>
int small(int a, int b) { return a + b; }
int variadic(int n, ...)
{
    va_list ap; va_start(ap, n);
    int r = va_arg(ap, int);
    va_end(ap);
    return r;
}
double fp(double x) { return x * 2.0; }
int big(int n)
{
    int t = 0;
    t += n; t += n*2; t += n*3; t += n*4; t += n*5; t += n*6;
    t += n*7; t += n*8; t += n*9; t += n*10; t += n*11; t += n*12;
    return t;
}
int caller(int n)
{
    return small(n, 1) + variadic(n, 2) + (int)fp((double)n) + big(n);
}
EOF

# ---- one cause, one reason ------------------------------------------------
"$EMBCC" why not-inlined "$out/p.c" -O2 > "$out/why.txt" 2>&1
sed "s|$out/||" "$out/why.txt"
for pair in "variadic:callee-is-varargs" "fp:returns-floating-point" \
            "big:callee-too-large"; do
    fn=${pair%%:*}; reason=${pair#*:}
    grep -q "^$fn " "$out/why.txt" || { echo "FAIL: no remark for $fn"; exit 1; }
    grep -q "because $reason" "$out/why.txt" ||
        { echo "FAIL: $fn should be $reason"; exit 1; }
done
# Three refusals, three DIFFERENT reasons -- the whole point.
n=$(grep -c "^  because " "$out/why.txt")
u=$(grep "^  because " "$out/why.txt" | sed 's/ —.*//' | sort -u | wc -l | tr -d ' ')
[ "$n" = "$u" ] || { echo "FAIL: $n refusals collapsed into $u reasons"; exit 1; }
echo "$n refusals, $u distinct reasons: a cause is not lost on the way out"

# The size case carries the fact that settled it, not just a label.
grep -q "callee-too-large — .* instructions, budget 24" "$out/why.txt" ||
    { echo "FAIL: no supporting fact for the size decision"; exit 1; }
echo "and the fact that settled it: '$(sed -n 's/.*— \(.*instructions, budget [0-9]*\)/\1/p' \
     "$out/why.txt" | head -1)'"

# ---- a decision that went the other way -----------------------------------
"$EMBCC" why inlined "$out/p.c" -O2 > "$out/yes.txt" 2>&1
grep -q "^small " "$out/yes.txt" || { cat "$out/yes.txt"
    echo "FAIL: small() should have been inlined"; exit 1; }
grep -q "because small-enough" "$out/yes.txt" || { echo "FAIL: reason"; exit 1; }
echo "the decisions that succeeded are recorded too, not only the refusals"

# ---- the subject filter ---------------------------------------------------
"$EMBCC" why not-inlined big "$out/p.c" -O2 > "$out/one.txt" 2>&1
[ "$(grep -c "^  because " "$out/one.txt")" = 1 ] ||
    { cat "$out/one.txt"; echo "FAIL: subject filter"; exit 1; }
grep -q "^big " "$out/one.txt" || { echo "FAIL: wrong subject"; exit 1; }
echo "asking about one function answers about that one"

# ---- an answer that is honest about having none ---------------------------
# The inliner is an -O2 pass. At -O0 it never ran, so there is no decision to
# report -- and saying "no remarks" without saying why would read as "there
# was no reason".
"$EMBCC" why not-inlined "$out/p.c" -O0 > "$out/none.txt" 2>&1
grep -q "nothing recorded" "$out/none.txt" ||
    { cat "$out/none.txt"; echo "FAIL: should report nothing recorded"; exit 1; }
grep -q "needs -O2" "$out/none.txt" ||
    { echo "FAIL: should say why there is nothing"; exit 1; }
echo "at -O0 it says nothing was recorded, and why -- not an empty list"

# ---- remarks during an ordinary compile -----------------------------------
"$EMBCC" -c "$out/p.c" -O2 -fremarks -o "$out/p.o" 2> "$out/rem.txt"
[ -f "$out/p.o" ] || { echo "FAIL: -fremarks suppressed the object"; exit 1; }
grep -q "remark: inlined 'small'.*\[inline/small-enough\]" "$out/rem.txt" ||
    { cat "$out/rem.txt"; echo "FAIL: -fremarks text form"; exit 1; }
echo "-fremarks during a real compile: the object is still written"

# Off by default: a pass that always built strings would cost every compile.
"$EMBCC" -c "$out/p.c" -O2 -o "$out/p2.o" 2> "$out/quiet.txt"
[ ! -s "$out/quiet.txt" ] ||
    { cat "$out/quiet.txt"; echo "FAIL: remarks leaked without -fremarks"; exit 1; }
echo "and silent without it"

# ---- data first, text second (§13) ----------------------------------------
"$EMBCC" -c "$out/p.c" -O2 -fremarks=json -o "$out/p3.o" 2> "$out/rem.json"
if command -v python3 > /dev/null 2>&1; then
    python3 - "$out/rem.json" << 'PY' || exit 1
import json, sys
rs = json.load(open(sys.argv[1]))
assert rs, "no remarks in the JSON"
for r in rs:
    for k in ("pass", "decision", "subject", "reason"):
        assert k in r, (k, r)
inl = [r for r in rs if r["decision"] == "inlined"]
assert any(r["subject"] == "small" for r in inl), rs
byr = {r["reason"] for r in rs if r["decision"] == "not-inlined"}
assert len(byr) >= 3, byr
loc = [r for r in rs if "location" in r]
assert loc and loc[0]["location"]["line"] > 0, rs
print("JSON: %d remarks, %d distinct not-inlined reasons, with locations"
      % (len(rs), len(byr)))
PY
else
    echo "skipped the JSON check: no python3"
fi
