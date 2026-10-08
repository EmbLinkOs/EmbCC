#!/bin/sh
# SPARC delay slots, by their SHAPE in llvm-objdump's reading of EmbCC's
# objects (src/arch/sparc/codegen.c, take_slot): every transfer's slot
# holds an earlier instruction when one may run there, and never one that
# may not --
#
#   - a conditional branch reads the condition codes, so no cmp, tst or
#     other cc-setting instruction is in its slot (unless it is annulled:
#     then the slot runs only once the branch is taken, as a copy of its
#     target's first instruction does);
#   - a call writes %o7 before its slot runs, so the slot of a call (to a
#     symbol or through a register) neither reads nor writes %o7;
#   - a jump or call through a register reads it, so the slot never
#     writes that register.
#
# The objects are tests/golden/sparc-exec/slots.c -- whose functions are
# built around those hazards, and which sparc-exec.sh runs on the board --
# at every level, and a few lib/libc sources at -Os. The slots must also
# be FILLED: lib/libc's sources have far fewer nops than with the filling
# off (EMBCC_SPARC_NO_FILL), and the folds that share take_slot's rules
# are there -- a leaf function's retl, a return value moved by the
# restore, an annulled if/else arm.
set -u
echo "TEST-MARKER sparc-slots"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
T=sparc-none-elf
out=tests/golden/out/sparc-slots
rm -rf "$out"; mkdir -p "$out"

command -v "$OD" >/dev/null 2>&1 || { echo "skipped: $OD not found"; exit 0; }

# dis OBJ: the instructions, one per line: "mnemonic operands"
dis() {
    "$OD" -d --no-show-raw-insn "$1" |
        sed -n 's/^ *[0-9a-f]*:[[:space:]]*//p' | sed 's/[[:space:]]\{1,\}/ /g'
}

# check FILE: every slot obeys the three rules; prints the offenders
check() {
    awk '
    function mn(l) { split(l, a, " "); return a[1] }
    function ops(l) { sub(/^[^ ]* ?/, "", l); return l }
    function lastop(l,   o, n, a) { o = ops(l); n = split(o, a, ", *"); return a[n] }
    {
        m = mn($0)
        if (pm ~ /^b(e|ne|l|le|g|ge|lu|leu|gu|cs|cc|neg|pos|vs|vc)$/ &&
            (m == "cmp" || m == "tst" || m == "btst" || m ~ /cc$/))
            print "a cc-setting instruction in a conditional branch slot: " p " / " $0
        if (pm == "call" && $0 ~ /%o7/)
            print "%o7 in a call slot: " p " / " $0
        if ((pm == "jmp" || pm == "call") && ops(p) ~ /^%[a-z][0-9]$/ &&
            m !~ /^st/ && lastop($0) == ops(p))
            print "a jump target written in its own slot: " p " / " $0
        p = $0; pm = m
    }' "$1"
}

srcs="lib/libc/src/string/string.c lib/libc/src/stdlib/strtol.c
lib/libc/src/stdio/format.c lib/libc/src/stdio/stdio.c
lib/libc/src/stdlib/qsort.c lib/libc/src/time/time.c"
fail=0
for opt in -O0 -O1 -O2 -Os; do
    o="$out/slots$opt"
    "$EMBCC" --target=$T $opt -c tests/golden/sparc-exec/slots.c -o "$o.o" ||
        { echo "slots.c does not compile at $opt"; exit 1; }
    dis "$o.o" > "$o.dis"
    check "$o.dis" > "$o.bad"
    [ -s "$o.bad" ] && { echo "slots.c $opt:"; head -5 "$o.bad"; fail=1; }
done
nf=0; nn=0; nt=0
for f in $srcs; do
    [ -f "$f" ] || continue
    b=$(basename "$f" .c)
    "$EMBCC" --target=$T -Os -Ilib/libc/include -Ilib/libc/src/math -c "$f" \
        -o "$out/$b.o" &&
    EMBCC_SPARC_NO_FILL=1 "$EMBCC" --target=$T -Os -Ilib/libc/include \
        -Ilib/libc/src/math -c "$f" -o "$out/$b-nofill.o" ||
        { echo "$f does not compile"; exit 1; }
    dis "$out/$b.o" > "$out/$b.dis"
    dis "$out/$b-nofill.o" > "$out/$b-nofill.dis"
    check "$out/$b.dis" > "$out/$b.bad"
    [ -s "$out/$b.bad" ] && { echo "$f:"; head -5 "$out/$b.bad"; fail=1; }
    nf=$((nf + $(grep -c '^nop$' "$out/$b.dis")))
    nn=$((nn + $(grep -c '^nop$' "$out/$b-nofill.dis")))
    nt=$((nt + $(grep -c -E '^(b[a-z]*(,a)?|call|jmp|ret|retl) ' "$out/$b-nofill.dis")))
done
[ "$fail" = 0 ] || exit 1
echo "no slot holds a cc-setter after a conditional branch, %o7 after a call, or a jump's own target"

# filled: at least a third of the nops are gone
[ "$nn" -gt 0 ] && [ $((nf * 3)) -le $((nn * 2)) ] || {
    echo "the slots are not filled: $nf nops, $nn with the filling off"; exit 1; }
echo "lib/libc's sources: $nf nops where the filling off leaves $nn ($nt transfers)"

# the folds: a leaf (retl, no save), the restore that moves the value, an
# annulled arm
d="$out/slots-O2.dis"
awk '/^retl/ { r++ } END { exit !r }' "$d" ||
    { echo "no leaf function (retl) in slots.c at -O2"; exit 1; }
grep -q '^restore %[a-z0-9]*, [^,]*, %o0$' "$out/stdio.dis" "$out/format.dis" ||
    { echo "no return value moved by its restore in lib/libc"; exit 1; }
grep -q '^b[a-z]*,a ' "$d" ||
    { echo "no annulled if/else arm in slots.c at -O2"; exit 1; }
echo "leaf functions return with retl, a restore moves the return value, an if/else arm is annulled"
