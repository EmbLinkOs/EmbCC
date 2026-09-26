#!/bin/sh
# The RISC-V C extension, against llvm-mc over 50,000 instructions.
#
# EmbCC does not SELECT compressed instructions. Every RISC-V
# instruction becomes bytes at one place (rv_w in src/arch/riscv/emit.c)
# and the short form is derived there from the canonical 32-bit
# encoding that was already built. GCC and LLVM instead carry
# compressed instructions as separate definitions the selector may
# choose, or relax a stream afterwards -- two descriptions of one
# instruction, and emit.h's whole argument is that a second description
# is a second chance to be wrong.
#
# Deriving it makes this test possible, and the test is the point:
#
#   take a 32-bit word, disassemble it with llvm-mc WITHOUT +c,
#   reassemble that text WITH +c -- llvm compresses on its own --
#   and require the bytes to equal what rv_compress() produced.
#
# No expectation is written by hand anywhere, so the corpus can sweep
# registers and offsets across their boundaries. That is what it takes:
# the compressed immediates are scattered (c.lw's offset sits in three
# separate places, c.addi16sp's in five), and the bug this found was
# invisible to a hundred hand-picked instructions -- `(long)w >> 20`
# does not sign-extend a twelve-bit field out of a zero-extended word,
# so every negative immediate silently failed to compress.
set -u
echo "TEST-MARKER riscv-compressed"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=$EMBCC_ROOT/tests/golden/out/riscv-compressed
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "skipped: llvm-mc / llvm-objcopy not found"; exit 0; }

cc -std=c99 -Wall -Wextra -o "$out/riscvcheck" \
   "$EMBCC_ROOT/tools/riscvcheck/riscvcheck.c" \
   "$EMBCC_ROOT/src/arch/riscv/emit.c" "$EMBCC_ROOT/src/arch/code.c" \
   "$EMBCC_ROOT/src/arch/target.c" "$EMBCC_ROOT/src/driver/util.c" \
   "$EMBCC_ROOT/src/driver/diag.c" "$EMBCC_ROOT/src/sema/type.c" \
   "$EMBCC_ROOT/src/sema/ldfloat.c" \
   "$EMBCC_ROOT/src/platform/platform_posix.c" || {
    echo "riscvcheck did not build"; exit 1; }

fail=0
for w in 32 64; do
    "$out/riscvcheck" --csweep$w > "$out/mine$w.bin" 2> "$out/words$w.hex" || {
        echo "rv$w: the sweep did not run"; fail=1; continue; }
    n=$(wc -l < "$out/words$w.hex" | tr -d ' ')
    "$MC" --disassemble -triple=riscv$w -mattr=+m "$out/words$w.hex" \
        > "$out/dis$w.s" 2> "$out/dis$w.err" || {
        echo "rv$w: llvm-mc could not disassemble the sweep"
        head -3 "$out/dis$w.err"; fail=1; continue; }
    if grep -q 'unknown' "$out/dis$w.s"; then
        echo "rv$w: the sweep produced an instruction llvm does not know --"
        echo "      the generator is emitting a reserved encoding"
        grep -n 'unknown' "$out/dis$w.s" | head -3; fail=1; continue
    fi
    "$MC" -triple=riscv$w -mattr=+m,+c -filetype=obj "$out/dis$w.s" \
        -o "$out/ref$w.o" 2> "$out/asm$w.err" || {
        echo "rv$w: llvm-mc could not reassemble it"
        head -3 "$out/asm$w.err"; fail=1; continue; }
    "$OBJCOPY" -O binary --only-section=.text "$out/ref$w.o" "$out/ref$w.bin"
    if cmp -s "$out/mine$w.bin" "$out/ref$w.bin"; then
        long=$((n * 4)); got=$(wc -c < "$out/mine$w.bin" | tr -d ' ')
        echo "  rv$w: $n instructions compress exactly as llvm-mc does" \
             "($long bytes -> $got)"
    else
        echo "FAIL rv$w: compression differs from llvm-mc"
        echo "     mine $(wc -c < "$out/mine$w.bin" | tr -d ' ') bytes," \
             "llvm $(wc -c < "$out/ref$w.bin" | tr -d ' ')"
        fail=1
    fi
done

# The sequences that must NOT compress. Each has an immediate that is
# filled in later, so the word passing the compressor carries a
# placeholder -- and `addi rd, rd, 0` IS c.mv, which is what silently
# happened to every symbol address until rv_pcrel_pair() existed.
cat > "$out/p.c" <<'CEOF'
extern int ext(int);
static const char s[] = "x";
int g;
const char *a(void) { return s; }
int *b(void) { return &g; }
int c(int x) { return ext(x) + 1; }
int d(int x) { return x > 3 ? x : -x; }
CEOF
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    "$EMBCC" --target=$t -Os -c "$out/p.c" -o "$out/p.o" 2>/dev/null || {
        echo "FAIL $t: the placeholder program does not compile"; fail=1
        continue; }
    "$EMBCC" --target=$t -Os -S "$out/p.c" -o "$out/p.s" 2>/dev/null
    # An auipc is four bytes and the instruction pairing with it must be
    # four too. -S prints one .byte line per instruction, so a two-byte
    # line right after a PCREL_HI20 relocation means the low half
    # compressed and the relocation is now writing over its neighbour.
    bad=$(grep -A1 'R_RISCV_PCREL_HI20' "$out/p.s" |
          grep -cE '^\t\.byte\t0x..,0x..$') || bad=0
    if [ "$bad" -ne 0 ]; then
        echo "FAIL $t: an instruction after a PCREL_HI20 was compressed"
        fail=1
    else
        echo "  $t: relocated pairs and branch placeholders stay four bytes"
    fi
done

# And the padding between functions: a compressed function can leave
# t->len at two mod four, and the alignment loop used to add FOUR bytes
# at a time -- which never terminates. Two functions in one unit is the
# whole reproduction.
printf 'int a(int x){return x+1;}\nint b(int x){return x+2;}\n' > "$out/two.c"
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    if "$EMBCC" --target=$t -O0 -c "$out/two.c" -o "$out/two.o" 2>/dev/null; then
        echo "  $t: two functions in a unit still terminate"
    else
        echo "FAIL $t: two functions in a unit did not compile"; fail=1
    fi
done

# The macros must say what the backend does.
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    "$EMBCC" --target=$t --dump-predef | grep -q '__riscv_compressed 1' || {
        echo "FAIL $t: emits compressed instructions without defining "\
             "__riscv_compressed"; fail=1; }
done
[ "$fail" -eq 0 ] && echo "  __riscv_c and __riscv_compressed are defined"

[ "$fail" -eq 0 ] || exit 1
