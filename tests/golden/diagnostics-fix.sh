#!/bin/sh
# Fix-its that are edits, not prose (docs/tools/diagnostics.md T3):
# -fdiagnostics-parseable-fixits prints them in GCC's line format, and
# --fix performs them. The test that means something is the round trip:
# take a file that does not compile, run --fix, and require that the file
# now compiles and says what it was meant to say.
set -eu
echo "TEST-MARKER diagnostics-fix"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/diagnostics-fix
rm -rf "$out"; mkdir -p "$out"

# 1. GCC's parseable form: fix-it:"FILE":{LINE:COL-LINE:NEXT}:"TEXT"
cat > "$out/typo.c" << 'EOF'
int main(void)
{
    int count = 3;
    return cont + 1;
}
EOF
"$EMBCC" -fdiagnostics-parseable-fixits -c "$out/typo.c" -o "$out/t.o" \
    > "$out/typo.log" 2>&1 || true
grep -qE '^fix-it:".*typo\.c":\{4:12-4:16\}:"count"$' "$out/typo.log" || {
    echo "no parseable fix-it in GCC's form:"; grep fix-it "$out/typo.log"; exit 1; }
echo "parseable: fix-it:\"...\":{4:12-4:16}:\"count\""

# 2. --fix does it, and the file compiles afterwards.
"$EMBCC" --fix -c "$out/typo.c" > "$out/fix1.log" 2>&1 || true
grep -q "return count + 1;" "$out/typo.c" || {
    echo "--fix did not apply the suggestion:"; cat "$out/typo.c"; exit 1; }
"$EMBCC" -c "$out/typo.c" -o "$out/t.o" || {
    echo "the fixed file does not compile"; exit 1; }
echo "--fix: the misspelt name replaced; the file compiles"

# 3. A missing ';' is the one whose fix is never in doubt — and two of them
#    in a file are both reported (the parser carries on as if it were
#    there) and both fixed at once.
cat > "$out/semi.c" << 'EOF'
int f(void)
{
    int a = 1
    int b = 2
    return a + b;
}
EOF
n=$("$EMBCC" -c "$out/semi.c" -o "$out/s.o" 2>&1 | grep -c ": error:" || true)
[ "$n" -eq 2 ] || { echo "expected both missing ';' reported, got $n"; exit 1; }
"$EMBCC" -c "$out/semi.c" -o "$out/s.o" 2>&1 | grep -q "insert ';' here" || {
    echo "no insertion note"; exit 1; }
"$EMBCC" --fix -c "$out/semi.c" > "$out/fix2.log" 2>&1 || true
grep -q "applied 2 fixes" "$out/fix2.log" || {
    echo "--fix did not apply both:"; cat "$out/fix2.log"; exit 1; }
"$EMBCC" -c "$out/semi.c" -o "$out/s.o" || {
    echo "the fixed file does not compile:"; cat "$out/semi.c"; exit 1; }
echo "--fix: both missing ';' inserted; the file compiles"

# 3b. The suggestions that come from what the compiler knows, not from a
#     dictionary: the member this type actually has, `.` where the value is
#     a pointer, and the header the C library declares a name in.
cat > "$out/sug.c" << 'EOF'
struct P { int x; int label; };
int f(struct P *p) { return p.x; }
int g(struct P q) { return q->x; }
int h(struct P q) { return q.labl; }
int i(void) { char *p = malloc(4); return p ? 0 : 1; }
EOF
"$EMBCC" -c "$out/sug.c" -o "$out/sug.o" > "$out/sug.log" 2>&1 || true
grep -q "use '->' here" "$out/sug.log" || { echo "no -> suggestion"; exit 1; }
grep -q "use '.' here" "$out/sug.log" || { echo "no . suggestion"; exit 1; }
grep -q "did you mean 'label'?" "$out/sug.log" || { echo "no member suggestion"; exit 1; }
grep -q "'malloc' is declared in <stdlib.h>" "$out/sug.log" || {
    echo "no header suggestion:"; cat "$out/sug.log"; exit 1; }
"$EMBCC" --fix -c "$out/sug.c" > "$out/sug2.log" 2>&1 || true
grep -q "applied 4 fixes" "$out/sug2.log" || {
    echo "--fix did not apply all four:"; cat "$out/sug2.log"; exit 1; }
head -1 "$out/sug.c" | grep -q "#include <stdlib.h>" || {
    echo "the include was not added:"; head -2 "$out/sug.c"; exit 1; }
grep -q "return p->x;" "$out/sug.c" || { echo "-> not applied"; exit 1; }
grep -q "return q.x;" "$out/sug.c" || { echo ". not applied"; exit 1; }
grep -q "return q.label;" "$out/sug.c" || { echo "member not applied"; exit 1; }
echo "--fix: '->' for '.', the member meant, and the missing #include"

# 4. Nothing to fix is said, not done: a file whose error has no known
#    edit is left exactly as it was.
cat > "$out/hard.c" << 'EOF'
int g(void)
{
    struct Missing m;
    return m.x;
}
EOF
cp "$out/hard.c" "$out/hard.orig"
"$EMBCC" --fix -c "$out/hard.c" > "$out/fix3.log" 2>&1 || true
grep -q "nothing to fix automatically" "$out/fix3.log" || {
    echo "expected it to say there was nothing to fix:"; cat "$out/fix3.log"; exit 1; }
cmp -s "$out/hard.c" "$out/hard.orig" || { echo "--fix edited a file it should not have"; exit 1; }
echo "--fix: an error with no known edit leaves the file untouched"

# 5. A correct file is never touched either.
cat > "$out/ok.c" << 'EOF'
int ok(void) { return 0; }
EOF
cp "$out/ok.c" "$out/ok.orig"
"$EMBCC" --fix -c "$out/ok.c" > "$out/fix4.log" 2>&1 || true
cmp -s "$out/ok.c" "$out/ok.orig" || { echo "--fix edited a correct file"; exit 1; }
echo "--fix: a correct file is left alone"
