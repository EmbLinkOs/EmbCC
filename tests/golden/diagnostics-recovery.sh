#!/bin/sh
# Every reason the file does not compile, in one run (docs/tools/diagnostics.md T2):
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
    if (b > ) return 0;
    return a + b;
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

# 4. Semantic errors recover too, and that is most of them: a name that is
#    not declared, a member that does not exist, an argument that does not
#    fit — four in one run, across two functions. A name misspelt once is
#    reported once, however many times it is used.
cat > "$out/sem.c" << 'EOF2'
int strlen(const char *s);
struct P { int x; int y; };
int f(struct P p, int n)
{
    int a = undeclared_one + 1;
    int b = p.z;
    int c = strlen(p);
    return a + b + c + n + undeclared_one;
}
int g(void) { return missing_two(); }
EOF2
"$EMBCC" -c "$out/sem.c" -o "$out/sem.o" > "$out/sem.log" 2>&1 && {
    echo "the file with semantic errors compiled"; exit 1; }
[ "$(nerr "$out/sem.log")" -eq 4 ] || {
    echo "expected 4 semantic errors:"; cat "$out/sem.log"; exit 1; }
for want in "'undeclared_one' is not declared" "has no member 'z'" \
            "cannot convert struct P" "'missing_two' is not declared"; do
    grep -q "$want" "$out/sem.log" || {
        echo "missing: $want"; cat "$out/sem.log"; exit 1; }
done
[ "$(grep -c "'undeclared_one' is not declared" "$out/sem.log")" -eq 1 ] || {
    echo "the same name reported twice:"; cat "$out/sem.log"; exit 1; }
echo "four semantic errors in one run; a repeated name reported once"

# 5. -fmax-errors stops where it says.
"$EMBCC" -fmax-errors=2 -c "$out/three.c" -o "$out/three.o" \
    > "$out/max.log" 2>&1 || true
[ "$(nerr "$out/max.log")" -eq 2 ] || {
    echo "-fmax-errors=2 did not stop at 2:"; cat "$out/max.log"; exit 1; }
grep -q "due to -fmax-errors=2" "$out/max.log" || {
    echo "no -fmax-errors note:"; cat "$out/max.log"; exit 1; }
echo "-fmax-errors=2: two errors, then it stops and says so"

# 6. C++ recovers too: three independent errors in three functions are
#    three diagnostics, and what it lowered to never reaches the C stage.
cat > "$out/rec.cc" << 'EOF2'
struct P { int x; int y; };
int a(P p) { return p.z; }
int b(P p) { return p.x + undeclared_here; }
int c(P p) { return p.y; }
int d(P p) { return p.w; }
EOF2
NLX=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_NEWLIB" || echo "$X86_NEWLIB")
"$EMBCC" --target="$TARGET" -isystem "$NLX/include" -c "$out/rec.cc"     -o "$out/rec.o" > "$out/rec.log" 2>&1 && { echo "the broken C++ compiled"; exit 1; }
[ "$(nerr "$out/rec.log")" -eq 3 ] || {
    echo "expected 3 C++ errors:"; cat "$out/rec.log"; exit 1; }
for line in 2 3 5; do
    grep -q "rec.cc:$line:" "$out/rec.log" || {
        echo "nothing reported on C++ line $line:"; cat "$out/rec.log"; exit 1; }
done
grep -q "compilation terminated: 3 errors" "$out/rec.log" || {
    echo "no summary after the C++ errors"; exit 1; }
printf 'struct Q { int v; };
int ok(Q q) { return q.v; }
' > "$out/ok.cc"
"$EMBCC" --target="$TARGET" -isystem "$NLX/include" -c "$out/ok.cc" -o "$out/ok.o" || {
    echo "correct C++ stopped compiling"; exit 1; }
echo "C++: three errors in one run; correct C++ unaffected"

# 7. All of them reach an editor: the JSON carries one object per error.
if command -v python3 >/dev/null 2>&1; then
    "$EMBCC" -fdiagnostics-format=json -c "$out/three.c" -o "$out/three.o" \
        2> "$out/three.json" || true
    python3 - "$out/three.json" "$n" << 'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
errs = [x for x in d if x["kind"] == "error"]
assert len(errs) == int(sys.argv[2]), (len(errs), sys.argv[2])
lines = set(x["locations"][0]["caret"]["line"] for x in errs)
assert {2, 4, 6} <= lines, lines
print("json: %d errors, covering every broken line" % len(errs))
PY
fi
