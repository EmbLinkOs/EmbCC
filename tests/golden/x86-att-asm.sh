#!/bin/sh
# x86-64's AT&T assembler (src/arch/x86_64/att.c), against llvm-mc:
#
#   1. insns.txt -- some 250 instructions across the integer, control,
#      system, string and SSE forms, every addressing mode -- assembled by
#      EmbCC and by llvm-mc and disassembled: the same bytes, line by line;
#   2. symbols.s -- labels (numeric and named, forward and back), jumps
#      that are two bytes where they reach and five or six where they do
#      not, calls and jumps to symbols elsewhere, sym(%rip), $sym, GOTPCREL
#      and data words: the same bytes and the same relocation types at the
#      same offsets (the symbol a relocation names may differ -- llvm-mc
#      names .data+N where EmbCC names the label -- and both link alike);
#   3. sum.s linked with C and run on the x86-64 harness;
#   4. an inline template the original vocabulary does not cover --
#      labels, lock cmpxchg, an "m" operand, a %b modifier, SSE, asm goto
#      -- compiled and run.
set -u
echo "TEST-MARKER x86-att-asm"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
here=$PWD/tests/golden/x86-att-asm
out=$PWD/tests/golden/out/x86-att-asm
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
MC=$(command -v llvm-mc || ls /opt/homebrew/opt/llvm/bin/llvm-mc 2>/dev/null || true)
OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
[ -n "$MC" ] && [ -n "$OD" ] || { echo "SKIP: no llvm-mc / llvm-objdump"; exit 0; }

dis() { "$OD" -dr "$1" | grep -vE 'file format|^$|Disassembly|^[0-9a-f]+ <' |
        sed 's/^ *[0-9a-f]*:[[:space:]]*//'; }

# ---- 1 ----
printf '\t.text\nf:\n' > "$out/insns.s"
sed 's/^/\t/' "$here/insns.txt" >> "$out/insns.s"
"$EMBCC" --target=x86_64-linux-gnu -c "$out/insns.s" -o "$out/insns.o" \
    2> "$out/insns.err" || fail "EmbCC refused insns.txt: $(head -5 "$out/insns.err")"
"$MC" -triple=x86_64 -filetype=obj -o "$out/insns.ref.o" "$out/insns.s" ||
    fail "llvm-mc refused insns.txt"
# (the bytes and the instruction; a %rip operand's comment names an address)
dis "$out/insns.o" | sed 's/ *#.*//' > "$out/insns.dis"
dis "$out/insns.ref.o" | sed 's/ *#.*//' > "$out/insns.ref.dis"
n=$(wc -l < "$here/insns.txt" | tr -d ' ')
diff "$out/insns.ref.dis" "$out/insns.dis" > "$out/insns.diff" ||
    fail "the bytes differ from llvm-mc's (< llvm-mc, > EmbCC):
$(head -30 "$out/insns.diff")"
echo "$n instructions as llvm-mc encodes them"

# ---- 2 ----
"$EMBCC" --target=x86_64-linux-gnu -c "$here/symbols.s" -o "$out/sym.o" \
    2> "$out/sym.err" || fail "EmbCC refused symbols.s: $(head -5 "$out/sym.err")"
"$MC" -triple=x86_64 -filetype=obj -o "$out/sym.ref.o" "$here/symbols.s" ||
    fail "llvm-mc refused symbols.s"
for o in sym sym.ref; do
    dis "$out/$o.o" | sed -e 's/ *#.*//' -e 's/\(R_X86_64_[A-Z0-9_]*\).*/\1/' > "$out/$o.dis"
    "$OD" -r -j .data "$out/$o.o" | grep R_X86 | awk '{print $1, $2}' > "$out/$o.drel"
done
diff "$out/sym.ref.dis" "$out/sym.dis" > "$out/sym.diff" ||
    fail "symbols.s: bytes or relocations differ (< llvm-mc, > EmbCC):
$(head -30 "$out/sym.diff")"
diff "$out/sym.ref.drel" "$out/sym.drel" > "$out/drel.diff" ||
    fail "symbols.s: the data's relocations differ: $(cat "$out/drel.diff")"
grep -qE '^7[0-9a-f] ' "$out/sym.dis" && grep -qE '^0f 8[0-9a-f] ' "$out/sym.dis" ||
    fail "no two-byte and six-byte conditional jumps both: the relaxation"
echo "labels, relaxation, PLT32/PC32/GOTPCRELX/32/32S/64: llvm-mc's bytes and relocations"

# ---- 3 and 4: run ----
. tools/hostpaths.sh 2>/dev/null || true
H=tests/harness/x86_64
# the run links against the x86_64 newlib link.sh finds; a host without it
# (CI's runners) checks the encodings only
newlib=${EMBCC_X86_NEWLIB:-$HOME/cross/newlib-c99/x86_64-elf}
if [ ! -d "$newlib/lib" ]; then
    echo "  (the runs: no x86_64 newlib at $newlib, skipped)"
elif [ -x "$H/link.sh" ] && [ -x "$H/run.sh" ]; then
    "$EMBCC" --target=x86_64-elf -c "$here/sum.s" -o "$out/sum.o" || fail "sum.s"
    "$EMBCC" --target=x86_64-elf -O2 -c "$here/main.c" -o "$out/main.o" || fail "main.c"
    "$H/link.sh" -o "$out/prog" "$out/main.o" "$out/sum.o" > "$out/link.err" 2>&1 ||
        fail "the link: $(head -3 "$out/link.err")"
    "$H/run.sh" "$out/prog" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] || fail "the .s program exits $rc, not 42"
    echo "a .s function, linked with C, runs: a loop, a %rip load, a call through the PLT"
    cp "$here/inline.c" "$out/inline.c"
    for O in -O0 -O2; do
        "$EMBCC" --target=x86_64-elf $O -c "$out/inline.c" -o "$out/inline.o" ||
            fail "inline.c at $O"
        "$H/link.sh" -o "$out/inline" "$out/inline.o" > /dev/null 2>&1 || fail "inline.c's link"
        "$H/run.sh" "$out/inline" > /dev/null 2>&1
        rc=$?
        [ "$rc" = 42 ] || fail "inline.c at $O exits $rc, not 42"
    done
    echo "inline templates in GNU as's syntax -- labels, lock, m, %b, SSE, asm goto -- run"
fi
