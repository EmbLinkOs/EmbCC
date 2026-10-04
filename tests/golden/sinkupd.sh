#!/bin/sh
# An induction variable's update, moved next to the copy that ends it
# (pass_sinkupd).
#
# `b[m++] = c` computes m + 1 before the store reads m, so phi destruction
# leaves `m1 = m + 1; ...; b[m] = c; ...; m = mov m1` and m and m1 are
# both alive from the add to the store. They cannot share a register, and
# the loop pays a `mov` every trip. With the add moved down to sit right
# before `m = mov m1`, m1 is born where m dies and the two are one
# register: the copy is a move to itself.
set -eu
echo "TEST-MARKER sinkupd"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/sinkupd
rm -rf "$out"; mkdir -p "$out"

cat > "$out/rev.c" <<'EOF'
int rev(char *b, const char *t, int n)
{
    int m = 0;
    while (n)
        b[m++] = t[--n];
    return m;
}
EOF
# The add of #1 to m must come right before the copy that reads it, on
# every target: the shape is the optimizer's, not a backend's.
for t in thumbv7em-none-eabi riscv32-unknown-elf aarch64-elf x86_64-linux-gnu; do
    "$EMBCC" inspect ir --target=$t -O2 -fno-unroll -c "$out/rev.c" -o /dev/null \
        > "$out/$t.txt" 2>&1 || { echo "FAIL: could not compile for $t"
                                  cat "$out/$t.txt"; exit 1; }
    adj=$(awk '/= add\.[48]s? %[0-9]+, #1\t/ { split($1, d, "%"); want = d[2]; next }
               want != "" { if ($3 ~ /^mov/ && $4 == "%" want) { print "yes" }
                            want = "" }' \
              "$out/$t.txt" | head -1)
    [ "$adj" = yes ] || {
        echo "FAIL: on $t the counter's add is not next to the copy that ends"
        echo "      it, so the two cannot share a register:"
        cat "$out/$t.txt"; exit 1; }
done
echo "m + 1 sits right before m = mov, on Thumb, RV32, aarch64 and x86-64"

# And on a board the copy is gone: the loop's last instruction before the
# branch is the increment itself, written into m's own register.
"$EMBCC" --target=riscv32-unknown-elf -O2 -fno-unroll -c -o "$out/rev.o" \
    "$out/rev.c" 2> "$out/c.err" || { cat "$out/c.err"; exit 1; }
if command -v llvm-objdump > /dev/null 2>&1; then
    llvm-objdump -d --no-show-raw-insn "$out/rev.o" > "$out/rev.dis"
    nmv=$(awk '/<rev>:/ { on = 1 } on && /\tmv\t/ { n++ } END { print n + 0 }' "$out/rev.dis")
    nmv0=$(EMBCC_NO_SINKUPD=1 "$EMBCC" --target=riscv32-unknown-elf -O2 -fno-unroll \
               -c -o "$out/rev0.o" "$out/rev.c" &&
           llvm-objdump -d --no-show-raw-insn "$out/rev0.o" |
           awk '/<rev>:/ { on = 1 } on && /\tmv\t/ { n++ } END { print n + 0 }')
    [ "$nmv" -lt "$nmv0" ] || {
        echo "FAIL: RV32's rev has $nmv moves with the update moved and $nmv0"
        echo "      without; the loop's copy should have gone:"; cat "$out/rev.dis"; exit 1; }
    echo "RV32: one move fewer ($nmv0 -> $nmv), the one the loop ran every trip"
fi
