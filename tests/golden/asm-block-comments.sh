#!/bin/sh
# A comment in a file-scope asm block is not part of the line.
#
# lib/libc's setjmp files write each instruction as `.long` with its
# disassembly beside it in a comment. On AArch64 that comment is `//`
# (GNU as reads `#` there as an immediate), and EmbCC's block assembler
# for x86-64 and AArch64 (src/arch/x86_64/topasm.c) stripped `#`, `;` and
# `/*` but not `//`: the bytes were right, but each comma in the comment
# counted one more value, so every label after the line was four bytes
# per comma too far on -- `longjmp` sat past the end of .text.
#
# Here a block puts labels after data lines with the comment styles GNU
# as reads on the target (each row is what aarch64-elf-gcc and
# x86_64-elf-gcc assemble to the same symbols), and each label must sit
# where its bytes begin.
set -u
echo "TEST-MARKER asm-block-comments"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/asm-block-comments
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# at TRIPLE SOURCE: each of a, b, c, d defined at 0, 4, 8, 12
at() {
    printf '%s\n' "$2" > "$out/$1.c"
    "$EMBCC" --target=$1 -c "$out/$1.c" -o "$out/$1.o" || fail "$1: does not compile"
    llvm-nm "$out/$1.o" | awk '{print $3, $1}' | sort > "$out/$1.syms"
    printf 'a 0000000000000000\nb 0000000000000004\nc 0000000000000008\nd 000000000000000c\n' \
        > "$out/want"
    cmp -s "$out/$1.syms" "$out/want" || {
        echo "--- $1: got"; cat "$out/$1.syms"; fail "$1: a label is not where its bytes begin"; }
}

at aarch64-elf '__asm__(".globl a\n.globl b\n.globl c\n.globl d\n"
"a:  .long 0xa9005013   // stp x19, x20, [x0, #0]\n"
"b:  .long 0xa9015815   /* stp x21, x22, [x0, #16] */\n"
"c:  .long 0xd503201f   // nop, with, commas, here\n"
"d:  .long 0xd65f03c0   // ret\n");'
at x86_64-elf '__asm__(".globl a\n.globl b\n.globl c\n.globl d\n"
"a:  .long 0x90909090   # four nops, as data\n"
"b:  .long 0x90909090   /* a, b, c */\n"
"c:  .long 0x90909090   # x, y, z\n"
"d:  .long 0xc3909090   /* and, ret */\n");'
echo "asm-block-comments: labels after commented data lines sit where their bytes begin (aarch64, x86-64)"
