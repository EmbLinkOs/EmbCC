#!/bin/sh
# EmbCC's Renesas RX encoder (src/arch/rx/emit.c), against QEMU's decoder.
#
# There is no llvm-mc for RX, and no RX toolchain is assumed. The referee
# is the emulator itself: tools/rxcheck prints every form the backend can
# emit -- each operation with registers below and above r7 (the dsp:5
# forms reach only r0..r7), immediates at both ends of every li width and
# of #uimm4/#uimm8, displacements at the ends of each scaled field,
# branches at the ends of their reach -- as the line QEMU's RX
# disassembler prints for that instruction, beside the bytes emit.c made
# for it, both from one call. The bytes are loaded into qemu-system-rx
# and its monitor disassembles them (`x/Ni`); QEMU's decoder is the one
# the emulator EXECUTES, so it must read back the same instruction, from
# the same number of bytes, at every line.
#
# The mistakes this machine invites are VALID different instructions: a
# displacement left unscaled reads another word, `sub r1, r2, r3`'s
# sources swapped computes r1 - r2, a sign-extending mov.b where movu.b
# was meant, a li field one byte short truncates the constant. Each of
# those decodes and runs; only a referee sees it.
#
# When an rx-elf binutils is installed (EMBCC_RX_AS, or rx-elf-as on
# PATH) the third column, the same instructions in GNU as syntax, is
# assembled too and must give the same bytes -- which also checks that
# emit.c picked the shortest encoding, as GNU as does. Without one that
# part is skipped and says so.
#
# Then every encoder range check is provoked and must stop the process
# rather than truncate.
set -u
echo "TEST-MARKER rx-encoding"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_RX)"; exit 0; }

out=tests/golden/out/rx-encoding
rm -rf "$out"; mkdir -p "$out"
BASE=0x01800000             # where gdbsim's -kernel loader puts an image

cc -std=c99 -Wall -Wextra -o "$out/rxcheck" \
   tools/rxcheck/rxcheck.c src/arch/rx/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "rxcheck did not build"; exit 1; }

"$out/rxcheck" --vocab $BASE > "$out/vocab.txt" || {
    echo "rxcheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 2000 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

# The image: every instruction's bytes, back to back.
cut -d'|' -f2 "$out/vocab.txt" | tr -d '\n' | xxd -r -p > "$out/v.bin"
cut -d'|' -f1 "$out/vocab.txt" > "$out/want.txt"
printf 'x/%di %s\nquit\n' "$n" "$BASE" > "$out/cmds"
# -S: the CPU never runs; the monitor only reads memory.
sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-60}" sh -c \
    'exec "$0" -M gdbsim-r5f562n8 -kernel "$1" -S -display none \
        -serial null -monitor stdio < "$2"' \
    "$QEMU" "$out/v.bin" "$out/cmds" > "$out/qemu.raw" 2>&1
# "0x01800000:  fb 52 00 00 00 00      <TAB>mov.l<TAB>#0, r5" -> the
# address, then the text after the byte dump
tr -d '\r' < "$out/qemu.raw" | grep '^0x[0-9a-f]*:' |
    awk -F'\t' '{ a = substr($1, 1, index($1, ":") - 1);
                  t = $2; for (k = 3; k <= NF; k++) t = t "\t" $k;
                  print a "|" t }' > "$out/qemu.txt"
m=$(wc -l < "$out/qemu.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "QEMU disassembled $m instructions where there are $n:"
    tail -5 "$out/qemu.raw"; exit 1; }
cut -d'|' -f2- "$out/qemu.txt" > "$out/got.txt"

# Where each instruction should start, from the lengths emit.c gave them.
awk -F'|' -v base=$((BASE)) '{ printf "0x%08x\n", base + off;
                              off += length($2) / 2 }' "$out/vocab.txt" \
    > "$out/want-addr.txt"
cut -d'|' -f1 "$out/qemu.txt" > "$out/got-addr.txt"

paste -d'|' "$out/want-addr.txt" "$out/got-addr.txt" "$out/want.txt" \
    "$out/got.txt" "$out/vocab.txt" |
    awk -F'|' '$1 != $2 { print "  at " $1 ": QEMU starts an instruction at " $2 \
                          " -- the one before has the wrong length"; next }
               $3 != $4 { print "  " $5 " (" $6 "): QEMU reads it as `" $4 "`" }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings differ from what QEMU decodes:"
    head -20 "$out/diff.txt"
    exit 1
fi

# GNU as, when there is one.
AS=${EMBCC_RX_AS:-rx-elf-as}
gas=skipped
if command -v "$AS" >/dev/null 2>&1; then
    OBJCOPY=${EMBCC_RX_OBJCOPY:-$(dirname "$(command -v "$AS")")/rx-elf-objcopy}
    awk -F'|' '$3 != "-" { print "\t" $3 }' "$out/vocab.txt" > "$out/g.s"
    awk -F'|' '$3 != "-" { print $2 }' "$out/vocab.txt" > "$out/g-ours.txt"
    "$AS" -o "$out/g.o" "$out/g.s" 2> "$out/g.err" || {
        echo "GNU as rejected the vocabulary -- an entry claims an"
        echo "instruction that does not exist:"; head -6 "$out/g.err"; exit 1; }
    "$OBJCOPY" -O binary -j P "$out/g.o" "$out/g.bin" 2>/dev/null ||
        "$OBJCOPY" -O binary -j .text "$out/g.o" "$out/g.bin"
    xxd -p "$out/g.bin" | tr -d '\n' > "$out/g-theirs.hex"
    tr -d '\n' < "$out/g-ours.txt" > "$out/g-ours.hex"
    if ! cmp -s "$out/g-ours.hex" "$out/g-theirs.hex"; then
        # the first line whose bytes are not where GNU as put them
        awk -F'|' '$3 != "-"' "$out/vocab.txt" |
            awk -F'|' -v t="$(cat "$out/g-theirs.hex")" \
                '{ l = length($2); if (substr(t, off + 1, l) != $2) {
                     print "  " $3 ": ours " $2 ", GNU as " substr(t, off + 1, l + 4) "...";
                     exit }; off += l }'
        echo "GNU as encodes the vocabulary differently (above: the first)"
        exit 1
    fi
    g=$(wc -l < "$out/g-ours.txt" | tr -d ' ')
    gas="$g of them byte for byte as GNU as encodes them"
fi

k=$("$out/rxcheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/rxcheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: rx:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n RX encodings decode in QEMU as the instruction meant, from the"
echo "length emit.c gave them ($gas);"
echo "and $k encoder range checks each refuse rather than truncate"
