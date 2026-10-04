#!/bin/sh
# Rotation of a loop whose test is reached through a store:
# `while ((d[n] = s[n]) != 0) n++;` -- strcpy, and the hash table's key
# copy. The header is the load, the store and the test, and duplicating it
# is what rotation does to every header: the guard runs it once and the
# latch once per iteration, the N+1 times the header ran. A store may
# appear twice on that argument as a load may. -Os leaves it: the guard is
# a second copy of the store for the two bytes of the jump it saves.
#
# What the copy must NOT then do is hand the latch's address to strength
# reduction's walking pointer, which has not moved yet at that point:
# tests/exec/rotated-index.c runs the loop.
set -eu
echo "TEST-MARKER rotate-store"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/rotate-store
rm -rf "$out"; mkdir -p "$out"

cat > "$out/c.c" <<'EOF'
int cpy(char *d, const char *s)
{
    int n = 0;
    while ((d[n] = s[n]) != 0)
        n++;
    return n;
}
EOF
backjmp() {                     # backjmp IRFILE -> a jmp to a label above it
    awk '/^L[0-9]+:/ { sub(":", "", $1); seen[$1] = 1; next }
         /jmp L[0-9]+/ { for (i = 1; i <= NF; i++)
                             if ($i ~ /^L[0-9]+$/ && seen[$i]) { print $i; exit } }' "$1"
}
for O in -O2 -Os; do
    "$EMBCC" inspect ir --target=aarch64-elf $O -c "$out/c.c" -o /dev/null \
        > "$out/ir$O.txt" 2>&1 || { echo "FAIL: could not compile at $O"
                                    cat "$out/ir$O.txt"; exit 1; }
done
[ -z "$(backjmp "$out/ir-O2.txt")" ] || {
    echo "FAIL: at -O2 the copy loop still jumps back to a top-tested header:"
    cat "$out/ir-O2.txt"; exit 1; }
echo "-O2: the copy loop is bottom-tested, its store duplicated into the latch"
[ -n "$(backjmp "$out/ir-Os.txt")" ] || {
    echo "FAIL: at -Os the copy loop should stay top-tested (no second store):"
    cat "$out/ir-Os.txt"; exit 1; }
echo "-Os: left top-tested, one copy of the store"
