#!/bin/sh
# The Xtensa code generator uses the density option's 16-bit forms --
# mov.n, movi.n, add.n, addi.n, l32i.n, s32i.n, retw.n, beqz.n, bnez.n --
# where they fit (a branch: tried narrow, three bytes when it did not reach),
# as GNU as narrows GCC's code (src/arch/xtensa/emit.h, xt_set_narrow).
# Every instruction was three bytes: lib/libc at -Os was 402507 bytes
# of functions, 321869 with them. EMBCC_NO_DENSITY=1 keeps three. -S
# writes one .byte line per instruction (target_insn_len), so a 2-byte
# line is a narrow one. That the narrow code runs is xtensa-exec's: a
# branch over a `movi d, 0` that became two bytes once landed inside the
# next instruction, in 148 of 387 programs.
set -u
echo "TEST-MARKER xtensa-density"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/xtensa-density
rm -rf "$out"; mkdir -p "$out"
cat > "$out/d.c" <<'EOC'
struct node { struct node *next, *prev; int key; };
int lt(int a, int b) { return a < b; }
int add3(int x) { return x + 3; }
int nz(const int *p) { if (!p) return 0; return *p + 1; }
void unlink_node(struct node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = 0;
    n->prev = 0;
}
int sum(const struct node *h, int k)
{
    int s = 1;
    for (const struct node *n = h->next; n != h; n = n->next)
        s += n->key + k;
    return s;
}
EOC
"$EMBCC" --target=xtensa-esp32-elf -Os -S "$out/d.c" -o "$out/n.s" || exit 1
EMBCC_NO_DENSITY=1 "$EMBCC" --target=xtensa-esp32-elf -Os -S "$out/d.c" \
    -o "$out/w.s" || exit 1
# (two zero bytes are the padding to the next function's alignment)
two() { grep -E '^[[:space:]]+\.byte[[:space:]]+0x[0-9a-f]+,0x[0-9a-f]+$' "$1" |
        grep -cv '0x00,0x00$'; }
three() { grep -cE '^[[:space:]]+\.byte[[:space:]]+0x[0-9a-f]+,0x[0-9a-f]+,0x[0-9a-f]+$' "$1"; }
n2=$(two "$out/n.s"); n3=$(three "$out/n.s"); w2=$(two "$out/w.s"); w3=$(three "$out/w.s")
[ "$w2" = 0 ] || { echo "FAIL: EMBCC_NO_DENSITY=1 still wrote $w2 narrow instructions"; exit 1; }
[ "$n2" -ge 8 ] || { echo "FAIL: only $n2 narrow instructions with the density option"; exit 1; }
nb=$((2 * n2 + 3 * n3)); wb=$((3 * w3))
[ "$nb" -lt "$wb" ] || { echo "FAIL: $nb bytes narrow, $wb wide"; exit 1; }
# each narrow form, by its op0 (the first byte's low nibble) and, for
# retw.n, its whole halfword
for f in 8:l32i.n 9:s32i.n a:add.n b:addi.n c:movi.n d:mov.n; do
    op=${f%%:*}; nm=${f#*:}
    grep -qE "^[[:space:]]+\\.byte[[:space:]]+0x[0-9a-f]?$op,0x[0-9a-f]+\$" "$out/n.s" || {
        echo "FAIL: no $nm"; exit 1; }
done
grep -qE '^[[:space:]]+\.byte[[:space:]]+0x1d,0xf0$' "$out/n.s" || { echo "FAIL: no retw.n"; exit 1; }
# beqz.n / bnez.n: op0 1100 with the t field's top bit set (movi.n has it
# clear), relaxed to beqz when the target is out of its 0..63 bytes
grep -qE '^[[:space:]]+\.byte[[:space:]]+0x[89a-f]c,0x[0-9a-f]+$' "$out/n.s" || {
    echo "FAIL: no beqz.n/bnez.n"; exit 1; }
echo "xtensa-density: $n2 of $((n2 + n3)) instructions narrow ($nb bytes, $wb without), every form; EMBCC_NO_DENSITY=1 none"
