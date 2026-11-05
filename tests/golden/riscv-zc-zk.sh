#!/bin/sh
# RISC-V's Zicond, Zbc, Zbkb, Zbkx, Zcb and Zcmp in EmbCC's assembler,
# against llvm-mc: each instruction assembled by both and disassembled by
# llvm-objdump, which must print the same bytes and the same text. Then
# the codegen that uses two of them: a select is czero.eqz/czero.nez with
# no branch under Zicond, and the compressor writes Zcb's 16-bit forms.
# (The exec corpus runs with them on the rv32k and rv64k boards,
# tests/golden/exec-boards.sh.)
set -u
echo "TEST-MARKER riscv-zc-zk"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/riscv-zc-zk
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
MC=$(command -v llvm-mc || ls /opt/homebrew/opt/llvm/bin/llvm-mc 2>/dev/null || true)
OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
[ -n "$MC" ] && [ -n "$OD" ] || { echo "SKIP: no llvm-mc / llvm-objdump"; exit 0; }

# one width: the statements, -march=, llvm's -mattr=
check() {
    tag=$1 triple=$2 march=$3 attr=$4
    printf '\t.text\nf:\n' > "$out/$tag.s"
    sed 's/^/\t/' "$out/$tag.list" >> "$out/$tag.s"
    "$EMBCC" --target="$triple" -march="$march" -c "$out/$tag.s" \
        -o "$out/$tag.o" 2> "$out/$tag.err" ||
        fail "$tag: EmbCC refused it: $(head -3 "$out/$tag.err")"
    "$MC" -triple="${triple%%-*}" -mattr="$attr" -filetype=obj \
        -o "$out/$tag.ref.o" "$out/$tag.s" 2> "$out/$tag.mcerr" ||
        fail "$tag: llvm-mc refused it: $(head -3 "$out/$tag.mcerr")"
    for o in "$tag" "$tag.ref"; do
        "$OD" -d --mattr="$attr" "$out/$o.o" | grep '^ *[0-9a-f]*:' |
            awk '{$1=""; print}' > "$out/$o.dis"
    done
    n=$(wc -l < "$out/$tag.list" | tr -d ' ')
    [ "$(wc -l < "$out/$tag.dis" | tr -d ' ')" = "$n" ] ||
        fail "$tag: $n statements, $(wc -l < "$out/$tag.dis") instructions"
    diff "$out/$tag.ref.dis" "$out/$tag.dis" > "$out/$tag.diff" ||
        fail "$tag: EmbCC's bytes differ from llvm-mc's (< llvm-mc, > EmbCC):
$(cat "$out/$tag.diff")"
    echo "$tag: $n instructions as llvm-mc encodes them"
}

cat > "$out/rv32.list" <<'EOF'
czero.eqz a0, a1, a2
czero.nez s0, t1, a5
clmul a0, a1, a2
clmulh t0, t1, t2
clmulr a3, a4, a5
pack a0, a1, a2
packh a0, a1, a2
brev8 a0, a1
zip a0, a1
unzip t3, t4
xperm4 a0, a1, a2
xperm8 a0, a1, a2
andn a0, a1, a2
rori a0, a1, 7
rev8 a0, a1
c.lbu a0, 3(a1)
c.lhu s0, 2(s1)
c.lh a0, 0(a1)
c.sb a5, 1(a4)
c.sh a0, 2(a1)
c.zext.b a0
c.sext.b s1
c.zext.h a0
c.sext.h a0
c.not a2
c.mul a0, a1
cm.push {ra}, -16
cm.push {ra, s0-s3}, -48
cm.push {ra, s0-s11}, -112
cm.pop {ra, s0}, 16
cm.popret {ra, s0-s11}, 112
cm.popretz {ra}, 32
cm.mva01s s0, s1
cm.mvsa01 s2, s7
EOF
check rv32 riscv32-unknown-elf rv32imac_zicond_zbc_zbkb_zbkx_zcb_zcmp_zbb \
    +m,+c,+zicond,+zbc,+zbkb,+zbkx,+zcb,+zcmp,+zbb

cat > "$out/rv64.list" <<'EOF'
packw a0, a1, a2
clmul a0, a1, a2
c.zext.w a0
cm.push {ra, s0-s11}, -112
cm.push {ra}, -16
cm.pop {ra, s0-s1}, 32
cm.popret {ra, s0-s2}, 48
EOF
check rv64 riscv64-unknown-elf rv64imac_zbkb_zbkc_zcb_zcmp_zba \
    +m,+c,+zbkb,+zbkc,+zcb,+zcmp,+zba

# the refusals: an instruction without its extension, Zcmp with Zcd
printf '\t.text\nf:\n\tczero.eqz a0, a1, a2\n' > "$out/no.s"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/no.s" -o "$out/no.o" \
    2> "$out/no.err" && fail "czero.eqz without zicond was accepted"
grep -q "needs zicond" "$out/no.err" || fail "czero without zicond: $(cat "$out/no.err")"
"$EMBCC" --target=riscv32-unknown-elf -march=rv32imafdc_zcmp -c "$out/no.s" \
    -o "$out/no.o" 2> "$out/zcd.err" && fail "zcmp with c and d was accepted"
grep -q "Zcmp and Zcd" "$out/zcd.err" || fail "zcmp+zcd: $(cat "$out/zcd.err")"
echo "an instruction without its extension, and Zcmp with Zcd, are refused"

# codegen: Zicond selects, Zcb's forms
cat > "$out/g.c" <<'EOF'
int sel(int c, int a, int b) { return c ? a : b; }
unsigned ld(unsigned char *p) { return p[1] + p[2]; }
unsigned short zh(unsigned x) { return (unsigned short)x; }
EOF
"$EMBCC" --target=riscv32-unknown-elf -march=rv32imac_zicond_zcb_zbb -O2 \
    -c "$out/g.c" -o "$out/g.o" || fail "the codegen sample does not compile"
"$OD" -d --mattr=+zicond,+zcb,+zbb,+c "$out/g.o" > "$out/g.dis"
sed -n '/<sel>:/,/^$/p' "$out/g.dis" > "$out/sel.dis"
grep -q czero.eqz "$out/sel.dis" && grep -q czero.nez "$out/sel.dis" ||
    fail "a select is not czero under Zicond: $(cat "$out/sel.dis")"
! grep -qE '[[:space:]](beq|bne|beqz|bnez)[[:space:]]' "$out/sel.dis" ||
    fail "the select still branches: $(cat "$out/sel.dis")"
# a 16-bit instruction's bytes: `814c` (LLVM 23) or `4c 81` (LLVM 18)
half='^ *[0-9a-f]+: ([0-9a-f]{4}|[0-9a-f]{2} [0-9a-f]{2})[[:space:]]+'
grep -E "${half}lbu" "$out/g.dis" > /dev/null ||
    fail "no 16-bit lbu (c.lbu) under Zcb: $(cat "$out/g.dis")"
grep -E "${half}zext.h" "$out/g.dis" > /dev/null ||
    fail "no 16-bit zext.h (c.zext.h) under Zcb: $(cat "$out/g.dis")"
echo "codegen: a select is czero.eqz/czero.nez with no branch; Zcb's 16-bit forms are used"

# Zcmp: a function that saves registers pushes and pops them with
# cm.push / cm.popret (a tail call's cm.pop); the slots are the spec's --
# ra BELOW the s-registers, which __builtin_return_address reads (the
# exec corpus's frame-builtins.c on the rv32k board runs it)
cat > "$out/p.c" <<'EOF2'
int g(int);
int f(int a, int b) { int x = g(a); int y = g(b + x); return x * y + g(x - y); }
EOF2
"$EMBCC" --target=riscv32-unknown-elf -march=rv32imac_zcmp -O2 \
    -c "$out/p.c" -o "$out/p.o" || fail "the Zcmp sample does not compile"
"$OD" -d --mattr=+zcmp,+c "$out/p.o" > "$out/p.dis"
grep -q 'cm.push[[:space:]]*{ra, s0-s1}, -16' "$out/p.dis" &&
    grep -q 'cm.popret[[:space:]]*{ra, s0-s1}, 16' "$out/p.dis" ||
    fail "no cm.push/cm.popret prologue under Zcmp: $(cat "$out/p.dis")"
! grep -qE '[[:space:]](sw|lw)[[:space:]]+(ra|s0|s1),' "$out/p.dis" ||
    fail "a save or restore beside cm.push/cm.popret: $(cat "$out/p.dis")"
echo "Zcmp: the saves are cm.push, the restores and return cm.popret"
