#!/bin/sh
# DWARF line info (D-010 step 1: address <-> source file:line). int $0x80
# aside, this is proven the honest way — a REAL debugger (gdb) reads the
# table on the host and resolves source lines to addresses in the right
# function. -g is opt-in: without it the object must be byte-for-byte as
# before (the M3 self-host fixed point depends on that), so that is asserted
# too, alongside determinism.
set -u
echo "TEST-MARKER debug-line"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/debug-line-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/dbg.c" <<'CEOF'
int add(int a, int b)
{
    int s = a + b;
    return s;
}
int main(void)
{
    int x = add(20, 22);
    return x;
}
CEOF

fail=0

# 1. -g emits the three DWARF sections.
"$EMBCC" --target="$TARGET" -g -c "$out/dbg.c" -o "$out/dbg.o" || { echo "embcc -g failed"; exit 1; }
sec=$(readelf -SW "$out/dbg.o" 2>/dev/null)
for s in .debug_abbrev .debug_info .debug_line; do
    echo "$sec" | grep -q "$s" || { echo "MISSING section: $s"; fail=1; }
done

# 2. The line table, decoded by a real DWARF reader, carries every statement
#    line. (Addresses are cross-checked by gdb below rather than hard-coded,
#    so the test does not break when codegen shifts an offset.)
dl=$(readelf --debug-dump=decodedline "$out/dbg.o" 2>/dev/null)
for ln in 3 4 8 9; do
    echo "$dl" | grep -qE "dbg\.c[[:space:]]+$ln[[:space:]]" \
        || { echo "MISSING line $ln in the decoded line table"; fail=1; }
done

# 3. A real debugger consumes it and attributes each line to the correct
#    FUNCTION — the actual acceptance, and offset-independent.
if command -v gdb >/dev/null 2>&1; then
    g=$(gdb -batch -nx "$out/dbg.o" \
            -ex "info line dbg.c:3" -ex "info line dbg.c:8" 2>/dev/null)
    echo "$g" | grep -qE 'Line 3 .*<add'  || { echo "gdb: line 3 not in add";  fail=1; }
    echo "$g" | grep -qE 'Line 8 .*<main' || { echo "gdb: line 8 not in main"; fail=1; }
    [ "$fail" -eq 0 ] && echo "gdb consumed the line table (line 3->add, line 8->main)"
else
    echo "gdb absent; skipped the debugger-consumes-it check (readelf still ran)"
fi

[ "$fail" -eq 0 ] && echo "debug-line: DWARF line info correct" || exit 1

# 4. -g output is deterministic (no timestamps / host paths baked in).
"$EMBCC" --target="$TARGET" -g -c "$out/dbg.c" -o "$out/dbg2.o"
cmp -s "$out/dbg.o" "$out/dbg2.o" || { echo "NONDETERMINISTIC -g output"; exit 1; }
echo "debug-line: -g output is byte-identical across runs"

# 5. Without -g, NO debug sections — default output is untouched, which is
#    what keeps the self-host fixed point.
"$EMBCC" --target="$TARGET" -c "$out/dbg.c" -o "$out/nog.o"
if readelf -SW "$out/nog.o" 2>/dev/null | grep -q '\.debug'; then
    echo "BUG: debug sections emitted without -g"; exit 1
fi
echo "debug-line: no -g -> no debug sections"

# 6. The rows are encoded as gcc and clang encode them: a special opcode
#    each where the line advance is -5..+8 (line_base -5, line_range 14,
#    opcode_base 13), const_add_pc or advance_pc in front of one for a
#    longer address advance, advance_line only outside that range. Every
#    row was advance_line + advance_pc + copy, five bytes and more. The
#    rows themselves are what step 2 and gdb check, and they must not
#    move: big.c's are its entry's line and each statement's, past a
#    gap of 31 lines and a statement of 30 stores, on each target.
DD=${EMBCC_LLVM_DWARFDUMP:-llvm-dwarfdump}
if command -v "$DD" > /dev/null 2>&1; then
    cat > "$out/big.c" <<'CEOF'
volatile int v[64];
int big(int a)
{
    v[0] = a;
    v[1] = a + 1;































    v[2] = a * 3; v[3] = a * 5; v[4] = a * 7; v[5] = a * 9; v[6] = a * 11; v[7] = a * 13; v[8] = a * 15; v[9] = a * 17; v[10] = a * 19; v[11] = a * 21; v[12] = a * 23; v[13] = a * 25; v[14] = a * 27; v[15] = a * 29; v[16] = a * 31; v[17] = a * 33; v[18] = a * 35; v[19] = a * 37; v[20] = a * 39; v[21] = a * 41; v[22] = a * 43; v[23] = a * 45; v[24] = a * 47; v[25] = a * 49; v[26] = a * 51; v[27] = a * 53; v[28] = a * 55; v[29] = a * 57; v[30] = a * 59; v[31] = a * 61;
    v[32] = a - 1;
    return v[a & 63];
}
CEOF
    for t in "$TARGET" thumbv7em-none-eabi riscv32-unknown-elf avr; do
        "$EMBCC" --target=$t -O0 -g -c "$out/big.c" -o "$out/big-$t.o" &&
        "$DD" -v --debug-line "$out/big-$t.o" > "$out/big-$t.dump" 2>&1 || {
            echo "big.c: -g or $DD failed for $t"; fail=1; continue; }
        rows=$(grep -cE '^ +0x[0-9a-f]{16} ' "$out/big-$t.dump")
        special=$(grep -cE '^0x[0-9a-f]{8}: [0-9a-f]{2} address \+= ' "$out/big-$t.dump")
        lines=$(awk '/^ +0x[0-9a-f]+ / { print $2 }' "$out/big-$t.dump" | sort -nu | tr '\n' ' ')
        # a row by copy follows only a zero advance: never advance_pc
        oldform=$(awk '/DW_LNS_advance_pc/ { pc = 1; next }
                       /DW_LNS_copy/ && pc { n++ } { pc = 0 }
                       END { print n + 0 }' "$out/big-$t.dump")
        [ "$special" -ge 5 ] && [ "$oldform" = 0 ] || {
            echo "$t: $rows rows, $special by a special opcode, $oldform by advance_pc + copy:"
            grep -E '^0x[0-9a-f]{8}: ' "$out/big-$t.dump" | head -12 | sed 's/^/     | /'
            fail=1; continue; }
        # every address advance decoded as encoded: the sequence ends
        # exactly at big's end (its size, from 0), which a special opcode
        # or const_add_pc carrying a wrong advance would move
        end=$(awk '/^ +0x[0-9a-f]+ .*end_sequence/ { print $1 }' "$out/big-$t.dump")
        size=$(llvm-nm -S "$out/big-$t.o" 2> /dev/null | awk '$4 == "big" { print $2 }')
        [ -n "$end" ] && [ -n "$size" ] && [ $((end)) = $((0x$size)) ] || {
            echo "$t: big's line sequence ends at '$end', the function is 0x$size bytes"; fail=1; }
        for ln in 2 4 5 37 38 39; do
            case " $lines " in *" $ln "*) ;; *)
                echo "$t: big.c's line table has no line $ln (has: $lines)"; fail=1 ;;
            esac
        done
    done
    [ "$fail" -eq 0 ] && echo "debug-line: rows by special opcodes (line_base -5, line_range 14, opcode_base 13), every line still there"
else
    echo "debug-line: $DD absent; skipped the special-opcode check"
fi
[ "$fail" -eq 0 ] || exit 1
echo "debug-line golden passed (running proof: gdb on the host)"
