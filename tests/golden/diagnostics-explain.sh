#!/bin/sh
# `embcc --explain` (docs/TOOLING.md T6): a diagnostic prints a stable id,
# and the id has an entry that says what the rule is, shows the mistake and
# the fix, and cites the standard. Rust has had this for years; no C
# compiler does.
#
# The invariant that keeps it honest is the last check: every id the
# compiler can print must have an entry, so the id in a diagnostic is never
# a dead end.
set -eu
echo "TEST-MARKER diagnostics-explain"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/diagnostics-explain
rm -rf "$out"; mkdir -p "$out"

fires() { # id source
    printf '%s\n' "$2" > "$out/case.c"
    "$EMBCC" -c "$out/case.c" -o "$out/case.o" > "$out/case.log" 2>&1 || true
    grep -q "\[$1\]" "$out/case.log" || {
        echo "$1 did not fire:"; cat "$out/case.log"; exit 1; }
    echo "  $1 on the code it is about"
}

echo "ids are printed with the diagnostic:"
fires E0001 'int f(void) { return undeclared_name; }'
fires E0002 'int f(void)
{
    int x = 5
    return x;
}'
fires E0004 'struct P { int x; };
int f(struct P p) { return p.z; }'
fires E0005 'int f(void) { struct Missing m; return 0; }'
fires E0008 'int sign(int n)
{
    if (n > 0) return 1;
    if (n < 0) return -1;
}'

# The entry itself: the rule, an example, the fix, the citation.
"$EMBCC" --explain E0001 > "$out/e1.txt" 2>&1
grep -q "E0001: a name is used that was never declared" "$out/e1.txt" || {
    echo "no title:"; head -3 "$out/e1.txt"; exit 1; }
grep -q "C99 6.5.1p2" "$out/e1.txt" || { echo "no citation"; exit 1; }
grep -q "embcc --fix" "$out/e1.txt" || { echo "does not say how to fix it"; exit 1; }
echo "--explain E0001: the rule, an example, the fix, the citation"

# Case does not matter, and the list is a list.
"$EMBCC" --explain e0004 | grep -q "^E0004:" || { echo "lowercase id rejected"; exit 1; }
"$EMBCC" --explain | grep -q "E0008" || { echo "--explain does not list them"; exit 1; }
if "$EMBCC" --explain E9999 > "$out/none.txt" 2>&1; then
    echo "an unknown id was accepted"; exit 1
fi
grep -q "no explanation for" "$out/none.txt" || { echo "unhelpful message"; exit 1; }
echo "--explain: case-insensitive, lists them, refuses an unknown id"

# An editor gets the id too.
printf 'struct P { int x; };\nint f(struct P p) { return p.z; }\n' > "$out/j.c"
"$EMBCC" -fdiagnostics-format=json -c "$out/j.c" -o "$out/j.o" 2> "$out/j.json" || true
if command -v python3 >/dev/null 2>&1; then
    python3 - "$out/j.json" << 'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
assert d[0].get("id") == "E0004", d[0]
print("json: the diagnostic carries \"id\": \"E0004\"")
PY
fi

# Every id the compiler can print has an entry: no dead ends.
ids=$(grep -rho '"E[0-9][0-9][0-9][0-9]"' src | tr -d '"' | sort -u)
[ -n "$ids" ] || { echo "no ids found in the sources"; exit 1; }
for id in $ids; do
    "$EMBCC" --explain "$id" > /dev/null 2>&1 || {
        echo "$id is printed by the compiler but has no entry"; exit 1; }
done
echo "every id the compiler prints has an entry ($(echo "$ids" | wc -l | tr -d ' ') of them)"
