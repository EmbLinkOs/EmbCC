#!/bin/sh
# RV32: 64-bit values and blocks are used where they live.
#
# A long long or a double is a register pair at RV32, and the lowerings
# used to move every operand into the scratch registers first: a 64-bit
# store from a pair to a pointer in a register was `mv t2, a0; mv t0, a2;
# mv t1, a3` before its two stores, a long long add eight moves around
# five instructions, a block copy `mv t1, a1; addi t2, sp, 48` before its
# first word, and a soft-float comparison branched on built its 0 or 1 in
# t0, moved it home and tested that. Now:
#   - loads, stores, add, subtract and the extension to 64 bits read and
#     write the pairs (and the address register) in place: no move into
#     t0-t3 in those functions;
#   - a block copy takes its bases where they are;
#   - a comparison helper's result is branched on directly: no sltz,
#     sgtz, seqz or snez in dbr/fbr, and a branch on a0 after each call.
# tests/exec/ldst64-inplace.c, the input, runs on the boards with the
# exec corpus and checks every value, NaN comparisons included.
set -u
echo "TEST-MARKER riscv-inplace"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
[ -n "${EMBCC_RA_MAXPOOL:-}" ] && { echo "SKIP: EMBCC_RA_MAXPOOL shrinks the pool"; exit 0; }
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
out=tests/golden/out/riscv-inplace
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
src=tests/exec/ldst64-inplace.c
n=0
for O in -O2 -Os; do
    "$EMBCC" --target=riscv32-unknown-elf $O -c "$src" -o "$out/f$O.o" ||
        fail "$O: compile"
    "$OBJDUMP" -d --no-show-raw-insn "$out/f$O.o" > "$out/f$O.dis"
    fn() { sed -n "/<$1>:/,/^\$/p" "$out/f$O.dis"; }
    for f in ld_first ld_second ld_idx st_pair st_idx cp_blk add_ext sub_pair; do
        fn $f > "$out/$f$O.dis"
        [ -s "$out/$f$O.dis" ] || fail "$O: no $f"
        if grep -qE '	mv	t[0-3], ' "$out/$f$O.dis"; then
            cat "$out/$f$O.dis"; fail "$O $f: an operand moved into a scratch register"
        fi
    done
    if fn cp_blk | grep -qE 'addi	t[0-6], '; then
        fn cp_blk; fail "$O cp_blk: a block copy's base moved into a scratch register"
    fi
    for f in dbr fbr dnot fnot; do
        fn $f > "$out/$f$O.dis"
        if grep -qE '	(sltz|sgtz|seqz|snez|slt|sltu)	' "$out/$f$O.dis"; then
            cat "$out/$f$O.dis"; fail "$O $f: a comparison's 0 or 1 was built"
        fi
        c=$(grep -cE 'jalr|	jal	' "$out/$f$O.dis")
        b=$(grep -cE '	(bltz|bgez|bgtz|blez|beqz|bnez|c\.beqz|c\.bnez)	a0, |	(blt|bge)	(zero, a0|a0, zero), ' "$out/$f$O.dis")
        case $f in dbr|fbr) want=6 ;; *) want=2 ;; esac
        [ "$c" = $want ] && [ "$b" = $want ] ||
            { cat "$out/$f$O.dis"; fail "$O $f: $c calls, $b branches on a0 (want $want and $want)"; }
    done
    n=$((n + 1))
done
echo "riscv-inplace: at $n levels 64-bit values, blocks and comparison results are used where they live"
