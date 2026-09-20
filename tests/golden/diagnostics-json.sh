#!/bin/sh
# Diagnostics an editor can act on (docs/tools/diagnostics.md T1): the same compile
# rendered as GCC's caret text or as GCC's -fdiagnostics-format=json, and a
# fix-it that is a real edit — a span of the source and the text to put
# there. The proof that the edit is real: applying it, mechanically, from
# the JSON alone makes the file compile.
set -eu
echo "TEST-MARKER diagnostics-json"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/diagnostics-json
rm -rf "$out"; mkdir -p "$out"
command -v python3 >/dev/null 2>&1 || { echo "skipped: python3 absent"; exit 0; }

cat > "$out/typo.c" << 'EOF'
int main(void)
{
    int count = 3;
    return cont + 1;
}
EOF

# 1. JSON: parses, and carries the diagnostic, its note, and the fix-it.
"$EMBCC" -fdiagnostics-format=json -c "$out/typo.c" -o "$out/typo.o" \
    2> "$out/typo.json" && { echo "the typo compiled"; exit 1; }
python3 - "$out/typo.json" << 'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
assert isinstance(d, list) and len(d) == 1, d
e = d[0]
assert e["kind"] == "error", e
assert "'cont' is not declared" in e["message"], e
loc = e["locations"][0]["caret"]
assert (loc["line"], loc["column"]) == (4, 12), loc
n = e["children"][0]
assert n["kind"] == "note" and "did you mean 'count'?" == n["message"], n
f = n["fixits"][0]
assert (f["start"]["line"], f["start"]["column"]) == (4, 12), f
assert (f["next"]["line"], f["next"]["column"]) == (4, 16), f
assert f["string"] == "count", f
print("json: error + note + fixit, at the right columns")
PY

# 2. The fix-it applied from the JSON alone — no English parsed — compiles.
python3 - "$out/typo.json" "$out/typo.c" "$out/fixed.c" << 'PY' || exit 1
import json, sys
diags, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(src).read().split("\n")
def fixits(d):
    for x in d.get("fixits", []):
        yield x
    for c in d.get("children", []):
        yield from fixits(c)
edits = [x for d in json.load(open(diags)) for x in fixits(d)]
assert edits, "no fix-it to apply"
for x in sorted(edits, key=lambda x: -x["start"]["column"]):
    i = x["start"]["line"] - 1
    a, b = x["start"]["column"] - 1, x["next"]["column"] - 1
    lines[i] = lines[i][:a] + x["string"] + lines[i][b:]
open(dst, "w").write("\n".join(lines))
PY
"$EMBCC" -c "$out/fixed.c" -o "$out/fixed.o" || {
    echo "the fixed file does not compile:"; cat "$out/fixed.c"; exit 1; }
grep -q "return count + 1;" "$out/fixed.c" || {
    echo "the fix-it edited the wrong span:"; cat "$out/fixed.c"; exit 1; }
echo "fixit applied from JSON: the file compiles"

# 3. Text is still text: heading, source line, caret — and the replacement
#    under it, where GCC prints it.
err=$("$EMBCC" -c "$out/typo.c" -o "$out/typo.o" 2>&1 || true)
echo "$err" | grep -q "typo.c:4:12: error:" || { echo "no located error:"; echo "$err"; exit 1; }
echo "$err" | grep -qE '^ +\^~+$' || { echo "no caret:"; echo "$err"; exit 1; }
echo "$err" | grep -qE '^ +count$' || { echo "no fix-it under the caret:"; echo "$err"; exit 1; }
echo "text: caret, and the replacement under it"

# 4. Colour is a choice, not a guess: never/always regardless of the pipe.
"$EMBCC" -fdiagnostics-color=always -c "$out/typo.c" -o "$out/typo.o" 2>&1 \
    | grep -q "$(printf '\033')" || { echo "-fdiagnostics-color=always: no colour"; exit 1; }
"$EMBCC" -fdiagnostics-color=never -c "$out/typo.c" -o "$out/typo.o" 2>&1 \
    | grep -q "$(printf '\033')" && { echo "-fdiagnostics-color=never: coloured"; exit 1; }
echo "colour: always and never both honoured"

# 5. Warnings: -w drops them, -Werror makes them errors AND the status.
cat > "$out/warn.c" << 'EOF'
#define FOO 1
#define FOO 2
int main(void) { return FOO - 2; }
EOF
w=$("$EMBCC" -c "$out/warn.c" -o "$out/warn.o" 2>&1); rc=$?
[ "$rc" -eq 0 ] || { echo "the warning was fatal"; exit 1; }
echo "$w" | grep -q "warning: macro 'FOO' redefined" || {
    echo "no redefinition warning:"; echo "$w"; exit 1; }
w=$("$EMBCC" -w -c "$out/warn.c" -o "$out/warn.o" 2>&1)
[ -z "$w" ] || { echo "-w did not silence it:"; echo "$w"; exit 1; }
if "$EMBCC" -Werror -c "$out/warn.c" -o "$out/warn.o" > "$out/we.log" 2>&1; then
    echo "-Werror still exited 0"; exit 1
fi
grep -q "error: macro 'FOO' redefined" "$out/we.log" || {
    echo "-Werror did not promote it:"; cat "$out/we.log"; exit 1; }
"$EMBCC" -w -Werror -c "$out/warn.c" -o "$out/warn.o" || {
    echo "-w should win over -Werror, as in gcc"; exit 1; }
echo "warnings: -w silences, -Werror promotes and fails the build"

# 6. A warning reaches the JSON as a warning, not as an error.
"$EMBCC" -fdiagnostics-format=json -c "$out/warn.c" -o "$out/warn.o" \
    2> "$out/warn.json" || { echo "the warning was fatal in json mode"; exit 1; }
python3 - "$out/warn.json" << 'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
assert [x["kind"] for x in d] == ["warning"], d
assert d[0]["locations"][0]["caret"]["line"] == 2, d
print("json: the warning, at its line")
PY
