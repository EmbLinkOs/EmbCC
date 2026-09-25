#!/bin/sh
# -fstack-usage: how much stack each function's own frame takes.
#
# The number a microcontroller's stack has to be sized from — there is
# no guard page there and nothing to grow into, so the deepest call path
# is added up ahead of time and has to fit. gcc's format exactly
# (`file:line:name<TAB>bytes<TAB>qualifier` in FILE.su), so the tools
# that already read those files read these.
set -u
echo "TEST-MARKER stack-usage"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/stack-usage
rm -rf "$out"; mkdir -p "$out"

cat > "$out/f.c" <<'EOF'
int leaf(int a) { return a + 1; }
int deep(int a)
{
    volatile int buf[64];
    for (int i = 0; i < 64; i++) buf[i] = a + i;
    return buf[a & 63];
}
int main(void) { return leaf(1) + deep(2); }
EOF

for t in x86_64-elf aarch64-elf thumbv7m-none-eabi; do
    "$EMBCC" --target=$t -O1 -fstack-usage -c "$out/f.c" -o "$out/f.o" || {
        echo "$t: -fstack-usage did not compile"; exit 1; }
    [ -f "$out/f.su" ] || { echo "$t: no .su file beside the object"; exit 1; }

    # gcc's three tab-separated columns, one line per function with code.
    awk -F'\t' 'NF != 3 { print "bad line: " $0; bad = 1 }
                END { exit bad }' "$out/f.su" || {
        echo "$t: .su is not gcc's format:"; cat "$out/f.su"; exit 1; }
    for fn in leaf deep main; do
        grep -q ":$fn	" "$out/f.su" || {
            echo "$t: $fn is missing from the report:"; cat "$out/f.su"
            exit 1; }
    done

    # `deep` holds 64 volatile ints it may not fold away, so its frame
    # must be at least those 256 bytes and must exceed `leaf`'s.
    d=$(awk -F'\t' '/:deep\t/ { print $2 }' "$out/f.su")
    l=$(awk -F'\t' '/:leaf\t/ { print $2 }' "$out/f.su")
    [ "$d" -ge 256 ] || {
        echo "$t: deep() reports $d bytes for a 256-byte array"; exit 1; }
    [ "$d" -gt "$l" ] || {
        echo "$t: deep() ($d) does not use more stack than leaf() ($l)"
        exit 1; }
    echo "$t: leaf $l bytes, deep $d bytes"
done
echo "-fstack-usage reports a frame per function on every target"
