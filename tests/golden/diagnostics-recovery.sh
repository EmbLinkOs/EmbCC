#!/bin/sh
# Every reason the file does not compile, in one run (docs/TOOLING.md T2):
# a syntax error is reported and the parser resumes at the next statement or
# declaration, so three independent mistakes are three diagnostics — not the
# first one and silence. The other half of the claim matters as much:
# recovery must not INVENT errors, so a file with one mistake still reports
# exactly one, and a correct file still compiles.
set -eu
echo "TEST-MARKER diagnostics-recovery"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/diagnostics-recovery
rm -rf "$out"; mkdir -p "$out"

nerr() { grep -c ": error:" "$1" || true; }

# 1. Three independent declarations, each broken; the good ones between them
#    still parse.
cat > "$out/three.c" << 'EOF'
int ok1(void) { return 1; }
int bad1(void) { return 1 + ; }
int ok2(void) { return 2; }
int bad2(void) { int x = ; return x; }
int ok3(void) { return 3; }
struct S { int a; int; };
EOF
"$EMBCC" -c "$out/three.c" -o "$out/three.o" > "$out/three.log" 2>&1 && {
    echo "the broken file compiled"; exit 1; }
n=$(nerr "$out/three.log")
[ "$n" -ge 3 ] || { echo "expected 3+ errors, got $n:"; cat "$out/three.log"; exit 1; }
for line in 2 4 6; do
    grep -q "three.c:$line:" "$out/three.log" || {
        echo "nothing reported on line $line:"; cat "$out/three.log"; exit 1; }
done
grep -q "compilation terminated: [0-9]* errors" "$out/three.log" || {
    echo "no summary line:"; cat "$out/three.log"; exit 1; }
echo "three broken declarations: $n errors, one per line, in one run"

# 2. Statement-level recovery: two mistakes inside ONE function body.
cat > "$out/body.c" << 'EOF'
int f(int n)
{
    int a = n * ;
    int b = n + 1;
    return a + * b;
}
EOF
"$EMBCC" -c "$out/body.c" -o "$out/body.o" > "$out/body.log" 2>&1 && {
    echo "the broken body compiled"; exit 1; }
[ "$(nerr "$out/body.log")" -ge 2 ] || {
    echo "statement recovery reported fewer than 2:"; cat "$out/body.log"; exit 1; }
grep -q "body.c:3:" "$out/body.log" && grep -q "body.c:5:" "$out/body.log" || {
    echo "not both statements:"; cat "$out/body.log"; exit 1; }
echo "two mistakes in one body: both reported"

# 3. Recovery invents nothing: one mistake is one error, and a correct file
#    still compiles to an object.
cat > "$out/one.c" << 'EOF'
int g(void)
{
    int a = 1
    return a;
}
EOF
"$EMBCC" -c "$out/one.c" -o "$out/one.o" > "$out/one.log" 2>&1 || true
[ "$(nerr "$out/one.log")" -eq 1 ] || {
    echo "one mistake reported more than once:"; cat "$out/one.log"; exit 1; }
cat > "$out/good.c" << 'EOF'
struct P { int x, y; };
static int sum(struct P p) { return p.x + p.y; }
int main(void) { struct P p = { 2, 3 }; return sum(p) == 5 ? 0 : 1; }
EOF
"$EMBCC" -c "$out/good.c" -o "$out/good.o" || {
    echo "a correct file stopped compiling"; exit 1; }
echo "one mistake: exactly one error; correct code unaffected"

# 4. -fmax-errors stops where it says.
"$EMBCC" -fmax-errors=2 -c "$out/three.c" -o "$out/three.o" \
    > "$out/max.log" 2>&1 || true
[ "$(nerr "$out/max.log")" -eq 2 ] || {
    echo "-fmax-errors=2 did not stop at 2:"; cat "$out/max.log"; exit 1; }
grep -q "due to -fmax-errors=2" "$out/max.log" || {
    echo "no -fmax-errors note:"; cat "$out/max.log"; exit 1; }
echo "-fmax-errors=2: two errors, then it stops and says so"

# 5. All of them reach an editor: the JSON carries one object per error.
if command -v python3 >/dev/null 2>&1; then
    "$EMBCC" -fdiagnostics-format=json -c "$out/three.c" -o "$out/three.o" \
        2> "$out/three.json" || true
    python3 - "$out/three.json" "$n" << 'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
errs = [x for x in d if x["kind"] == "error"]
assert len(errs) == int(sys.argv[2]), (len(errs), sys.argv[2])
lines = sorted(x["locations"][0]["caret"]["line"] for x in errs)
assert lines == sorted(set(lines)), lines
print("json: %d errors, one per line" % len(errs))
PY
fi
