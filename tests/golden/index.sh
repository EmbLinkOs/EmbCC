#!/bin/sh
# embidx — the cross-TU index (vision §8.2), and the one claim that makes
# it worth having: a header edit that changes no interface REBUILDS
# NOTHING, where -MD rebuilds every unit that includes it.
#
# Every check here is a question no single translation unit can answer.
set -eu
echo "TEST-MARKER index"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBIDX=${EMBIDX:-./embidx}
[ -x "$EMBIDX" ] || { echo "skipped: no embidx (make embidx)"; exit 0; }

out=$EMBCC_ROOT/tests/golden/out/index-$ARCH
rm -rf "$out"; mkdir -p "$out"
export EMBCC

hdr() { printf '%s\n' "$1" > "$out/hdr.h"; }

hdr 'struct P { int a; long b; };
int shared(struct P *);'
cat > "$out/a.c" << 'EOF'
#include "hdr.h"
int g;
int f(struct P *p) { return shared(p) + g; }
EOF
cat > "$out/b.c" << 'EOF'
#include "hdr.h"
int shared(struct P *p) { return p->a; }
EOF

idx="$out/p.embidx"
"$EMBIDX" build -o "$idx" -I"$out" "$out/a.c" "$out/b.c" > /dev/null

# 1. The index records what each unit was DERIVED FROM, which is what
#    §8.2 means by a fact knowing its inputs. Without it the hashes
#    below are unanchored: nothing could tell whether they still
#    describe what is on disk.
grep -q "file .* $out/hdr.h" "$idx" || {
    echo "FAIL: the index does not record the header it was derived from"
    exit 1; }
grep -q "provides .* c:@F@shared" "$idx" || {
    echo "FAIL: b.c's definition is not in the index"; exit 1; }
grep -q "uses .* c:@F@shared" "$idx" || {
    echo "FAIL: a.c's observation is not in the index"; exit 1; }
echo "the index records each unit's inputs, what it provides, what it uses"

n() { "$EMBIDX" stale "$idx" 2>&1 >/dev/null | sed 's/.*, \([0-9]*\) need.*/\1/'; }
r() { "$EMBIDX" stale "$idx" 2>/dev/null | wc -l | tr -d ' '; }

# 2. Nothing changed: nothing to do.
[ "$(r)" = 0 ] || { echo "FAIL: a rebuild with nothing changed"; exit 1; }

# 3. THE CLAIM. A comment in a shared header moves its file hash, so both
#    units are re-examined -- and changes no interface, so neither is
#    rebuilt. `-MD` rebuilds both.
hdr '/* a comment nobody can observe */
struct P { int a; long b; };
int shared(struct P *);'
[ "$(n)" = 0 ] || {
    echo "FAIL: a header comment caused a rebuild:"
    "$EMBIDX" stale "$idx"; exit 1; }
[ "$(n)" = 0 ] && [ "$("$EMBIDX" stale "$idx" 2>&1 >/dev/null |
    sed 's/ unit.*//')" = 2 ] || {
    echo "FAIL: the comment should still have been NOTICED (re-examined)"
    "$EMBIDX" stale "$idx" 2>&1 >/dev/null; exit 1; }
echo "a header comment: both units re-examined, neither rebuilt"

# 4. A layout change is the opposite: both must rebuild, because both
#    compiled the member offsets in.
hdr 'struct P { long b; int a; };
int shared(struct P *);'
[ "$(n)" = 2 ] || {
    echo "FAIL: a changed struct layout did not rebuild both:"
    "$EMBIDX" stale "$idx"; exit 1; }
echo "a changed struct layout: both rebuilt"

# 5. A body edit changes no hash at all and must still rebuild its own
#    unit -- and only that one.
hdr 'struct P { int a; long b; };
int shared(struct P *);'
cat > "$out/a.c" << 'EOF'
#include "hdr.h"
int g;
int f(struct P *p) { return shared(p) + g + 1; }
EOF
"$EMBIDX" stale "$idx" 2>/dev/null > "$out/stale.txt"
grep -q "a.c: rebuild (its own source changed)" "$out/stale.txt" || {
    echo "FAIL: a body edit did not rebuild its own unit:"
    cat "$out/stale.txt"; exit 1; }
grep -q "b.c" "$out/stale.txt" && {
    echo "FAIL: a body edit rebuilt an unrelated unit:"
    cat "$out/stale.txt"; exit 1; }
echo "a function body edit: its own unit only"

# 6. Navigation: where a declaration lives and who observes it.
"$EMBIDX" who 'c:@F@shared' "$idx" > "$out/who.txt"
grep -q "^provides $out/b.c" "$out/who.txt" || {
    echo "FAIL: who did not find the definition"; cat "$out/who.txt"; exit 1; }
grep -q "^uses     $out/a.c" "$out/who.txt" || {
    echo "FAIL: who did not find the user"; cat "$out/who.txt"; exit 1; }
echo "who: the definition and every observer of a declaration"

# 7. The question no single unit can ask. c.c carries a DRIFTED copy of
#    struct P -- a member dropped. Each unit compiles; the link succeeds;
#    the program is wrong. Only something holding all three units at once
#    can see it.
cat > "$out/c.c" << 'EOF'
struct P { int a; };
int shared(struct P *);
int drift(struct P *p) { return shared(p); }
EOF
cat > "$out/a.c" << 'EOF'
#include "hdr.h"
int g;
int f(struct P *p) { return shared(p) + g; }
EOF
"$EMBIDX" build -o "$out/q.embidx" -I"$out" "$out/a.c" "$out/b.c" "$out/c.c" \
    > /dev/null
if "$EMBIDX" check "$out/q.embidx" > "$out/check.txt" 2>&1; then
    echo "FAIL: the drifted struct was not reported:"; cat "$out/check.txt"
    exit 1
fi
grep -q "^conflict c:@S@P" "$out/check.txt" || {
    echo "FAIL: wrong report for the drifted struct:"; cat "$out/check.txt"
    exit 1; }
echo "check: two units disagreeing about one struct, which links and lies"

# 8. And a clean project is clean.
"$EMBIDX" check "$idx" > "$out/clean.txt" || {
    echo "FAIL: a consistent project was reported as broken:"
    cat "$out/clean.txt"; exit 1; }
grep -q "0 problems" "$out/clean.txt" || {
    echo "FAIL: expected no problems:"; cat "$out/clean.txt"; exit 1; }

# 9. Discardable and deterministic (§8.2, R4): rebuilt from the sources
#    it must be byte-identical, or two builds cannot be compared.
cp "$idx" "$out/first.embidx"
rm -f "$idx"
"$EMBIDX" build -o "$idx" -I"$out" "$out/a.c" "$out/b.c" > /dev/null
cmp -s "$out/first.embidx" "$idx" || {
    echo "FAIL: rebuilding the index did not reproduce it:"
    diff "$out/first.embidx" "$idx" | head; exit 1; }
echo "the index is discardable and rebuilds byte-identically"
