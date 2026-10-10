#!/bin/sh
# The aarch64 inline-asm assembler, refereed by aarch64-elf-as.
#
# Two sets of lines go through BOTH assemblers and must produce identical
# bytes: every template the EmbLinkOS ARM kernel's inline asm uses
# (tests/golden/aarch64/arm64-asm.s), and every entry of the assembler's own
# vocabulary tables — each named system register, tlbi operation, barrier
# option and hint — generated from the tables so none can escape the check.
set -eu
echo "TEST-MARKER arm64-asm"
. "$(dirname "$0")/../../lib.sh"

cd "$(dirname "$0")/../../.."
AS="${EMBCC_AARCH64_AS:-aarch64-elf-as}"
OBJCOPY="${EMBCC_AARCH64_OBJCOPY:-aarch64-elf-objcopy}"
OBJDUMP="${EMBCC_AARCH64_OBJDUMP:-aarch64-elf-objdump}"
CC="${CC:-cc}"
command -v "$AS" >/dev/null 2>&1 || {
    echo "arm64-asm: $AS not found — cannot referee the encodings" >&2; exit 1; }

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Strings, alignments, data and %c in a template, against llvm-mc for an
# ELF and a Mach-O object: .ascii/.asciz/.string, .p2align/.balign/.align
# with and without a fill and a maximum, .byte to .xword, and constants
# written in with %c0, as Linux's asm-offsets and EmbLinkRTOS's layout
# probes write them -- each was ".ascii is not supported" and "modifier
# '%c' is not supported". llvm-mc's bytes at every word phase in the
# section (tests/harness/asmdir.sh): the padding is decided where the
# template lands.
MC=${EMBCC_LLVM_MC:-llvm-mc}
LOBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
EMBCC=${EMBCC:-$PWD/embcc}
if command -v "$MC" >/dev/null 2>&1 && command -v "$LOBJCOPY" >/dev/null 2>&1
then
    cat > "$out/dirs.txt" <<'EOF'
.ascii "->EMB_PROBE s %c1 %c0"
.p2align 2
.ascii "abc"
.align 3
.asciz "hi", "x"
.byte 0x55
.p2align 4
.string "\t\"q\\\101\x42\0z"
.balign 8
.byte 1, 255, -128, %c0, %c1
.p2align 3, 0x5a
.byte 7
.p2align 4,,5
.byte 9
.p2align 4,,15
.ascii "a;b#c//d"
.hword 0x1234, %c0
.short 7
.2byte 9
.word 0x12345678
.4byte 1
.xword -2
.dword 3
.8byte 4
.p2align 2
EOF
    ( OBJCOPY=$LOBJCOPY
      . tests/harness/asmdir.sh
      asmdir_referee "$out" aarch64-elf "-triple=aarch64" nop 4 "$out/dirs.txt" &&
      asmdir_referee "$out" arm64-apple-darwin "-triple=aarch64-apple-darwin" \
          nop 4 "$out/dirs.txt" __TEXT,__text ) || exit 1
    printf 'int f(int x){ __asm__ volatile(".byte %%c0" : : "r"(x)); return x; }\n' \
        > "$out/c.c"
    if "$EMBCC" --target=aarch64-elf -c "$out/c.c" -o /dev/null 2> "$out/c.err"; then
        echo "arm64-asm: %c of a register operand was accepted"; exit 1
    fi
    grep -q "names a register operand" "$out/c.err" || {
        echo "arm64-asm: the refusal of %c on a register does not say why:"
        cat "$out/c.err"; exit 1; }
else
    echo "arm64-asm: SKIP the directives against llvm-mc: llvm-mc not found"
fi

$CC -std=c99 -Wall -Wextra -Werror -o "$out/check" \
    tools/a64check/a64asmcheck.c src/arch/aarch64/asm.c \
    src/arch/aarch64/emit.c src/arch/code.c src/driver/util.c \
    src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c

{ grep -v '^//' tests/golden/aarch64/arm64-asm.s; "$out/check" --vocabulary; } > "$out/lines"

"$out/check" < "$out/lines" > "$out/ours.bin"

# armv8.2-a: PAN (v8.1) is in the vocabulary.
"$AS" -march=armv8.2-a -o "$out/ref.o" "$out/lines"
"$OBJCOPY" -O binary -j .text "$out/ref.o" "$out/ref.bin"

if cmp -s "$out/ours.bin" "$out/ref.bin"; then
    echo "arm64-asm: $(wc -l < "$out/lines" | tr -d ' ') lines, $(( $(wc -c < "$out/ref.bin") / 4 )) instructions agree with $AS"
    exit 0
fi
echo "arm64-asm: EmbCC and $AS disagree:" >&2
for f in ours ref; do
    "$OBJCOPY" -I binary -O elf64-littleaarch64 -B aarch64 "$out/$f.bin" "$out/$f.elf"
    "$OBJDUMP" -D -m aarch64 "$out/$f.elf" | sed -n '/^ *[0-9a-f]*:	/p' > "$out/$f.dis"
done
diff "$out/ref.dis" "$out/ours.dis" | head -30 >&2
exit 1
