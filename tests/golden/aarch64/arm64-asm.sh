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

$CC -std=c99 -Wall -Wextra -Werror -o "$out/check" \
    tools/a64check/a64asmcheck.c src/arch/aarch64/asm.c \
    src/arch/aarch64/emit.c src/arch/code.c src/driver/util.c \
    src/driver/diag.c src/platform/platform_posix.c

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
