#!/bin/sh
# EmbLD carries DWARF into the image, so a debugger can open it.
#
# `-g` produced correct OBJECTS and an executable no debugger could
# read: DWARF is not SHF_ALLOC, EmbLD collected only allocated
# sections, and the debug information went nowhere. EmbLD wrote its own
# .embdbg sidecar instead, which embdbg reads and gdb does not --
# attaching gdb to a QEMU guest gave "No symbol table is loaded".
#
# Merging is simpler here than in general, and the reason is worth
# knowing: EmbCC's DWARF writer expresses every cross-reference AS A
# RELOCATION. A CU's abbrev offset and its stmt_list are absolute
# relocations against the .debug_abbrev and .debug_line section
# symbols, and its low_pc/high_pc against .text. So concatenating the
# sections and resolving those relocations against each object's own
# contribution rebases everything -- there is no DWARF-aware fixup.
#
# The test is a real gdb session against a QEMU guest, because that is
# the claim. An image with .debug_info in it that a debugger cannot use
# would pass every structural check.
set -u
echo "TEST-MARKER link-dwarf"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/link-dwarf
rm -rf "$out"; mkdir -p "$out"
fail=0
T=riscv32-unknown-elf
QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv32}

cat > "$out/prog.c" <<'CEOF'
void putn(long v); void puts_(const char *s);
struct point { int x, y; };
static int scale(struct point *p, int k)
{
    int wide = p->x * k;
    int tall = p->y * k;
    int total = wide + tall;
    return total;
}
int main(void)
{
    struct point pt = { 3, 4 };
    putn(scale(&pt, 6));
    puts_("\n==END==\n");
    return 0;
}
CEOF

export EMBCC_RISCV_HARNESS="$out"
for f in boot io; do
    "$EMBCC" --target=$T -g -c "$EMBCC_ROOT/tests/harness/riscv/$f.c" \
             -o "$out/$f.o" 2>/dev/null || {
        echo "FAIL: the harness does not compile with -g"; exit 1; }
done
"$EMBCC" --target=$T -g -O0 -c "$out/prog.c" -o "$out/prog.o" 2>/dev/null || {
    echo "FAIL: the program does not compile with -g"; exit 1; }
sh "$EMBCC_ROOT/tests/harness/riscv/link.sh" "$out/prog.elf" "$out/prog.o" \
    > "$out/ld.txt" 2>&1 || {
    echo "FAIL: the image does not link"; head -3 "$out/ld.txt" | sed 's/^/     | /'
    exit 1; }

# The sections reached the image, and three objects' worth were merged
# into one of each rather than appearing three times.
if command -v llvm-readelf > /dev/null 2>&1; then
    llvm-readelf -S "$out/prog.elf" > "$out/sec.txt" 2>/dev/null
    for want in .debug_info .debug_abbrev .debug_line; do
        n=$(grep -c "$want" "$out/sec.txt") || n=0
        [ "$n" -eq 1 ] || { echo "FAIL: $want appears $n times (want 1)"
                            fail=1; }
    done
    [ "$fail" -eq 0 ] && echo "  the image carries one merged .debug_* of each"
fi

# Structurally valid after merging -- a wrong abbrev or stmt_list offset
# is exactly what a rebase gets wrong, and this catches it.
if command -v llvm-dwarfdump > /dev/null 2>&1; then
    if llvm-dwarfdump --verify "$out/prog.elf" > "$out/vf.txt" 2>&1; then
        echo "  the merged DWARF verifies"
    else
        echo "FAIL: the merged DWARF does not verify"
        grep -iE 'error' "$out/vf.txt" | head -3 | sed 's/^/     | /'
        fail=1
    fi
fi

# And the claim: a debugger uses it.
if command -v gdb > /dev/null 2>&1 && command -v "$QEMU" > /dev/null 2>&1 &&
   gdb --batch -ex 'set architecture riscv:rv32' >/dev/null 2>&1; then
    port=${EMBCC_GDB_PORT:-3417}
    "$QEMU" -M virt -bios none -nographic -kernel "$out/prog.elf" \
        -S -gdb "tcp::$port" > "$out/q.log" 2>&1 &
    qpid=$!
    sleep 2
    gdb --batch -q "$out/prog.elf" \
        -ex 'set architecture riscv:rv32' \
        -ex "target remote :$port" \
        -ex 'break scale' -ex 'continue' \
        -ex 'info args' -ex 'next' -ex 'next' -ex 'next' \
        -ex 'info locals' -ex 'up' -ex 'info locals' > "$out/gdb.txt" 2>&1
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null

    # A breakpoint placed BY FUNCTION NAME and reported with a source
    # line means the line table and the DIEs both survived the merge.
    grep -q 'prog.c, line' "$out/gdb.txt" || {
        echo "FAIL: gdb could not place a breakpoint by source line"
        head -6 "$out/gdb.txt" | sed 's/^/     | /'; fail=1; }
    # Argument VALUES mean the frame base and the locations are right.
    grep -q 'k = 6' "$out/gdb.txt" || {
        echo "FAIL: gdb did not read the arguments (k = 6)"
        grep -A3 'info args' "$out/gdb.txt" | head -4 | sed 's/^/     | /'
        fail=1; }
    # `break scale` stops at its first statement, past the prologue
    # (the entry row and prologue_end), and three `next`s later the
    # locals hold what the statements computed.
    grep -q 'prog.c, line 5' "$out/gdb.txt" &&
        grep -q 'total = 42' "$out/gdb.txt" || {
        echo "FAIL: break scale did not stop at line 5, or the locals are wrong"
        grep -E 'line|wide|tall|total' "$out/gdb.txt" | head -5 | sed 's/^/     | /'
        fail=1; }
    # A struct local printed with its members is the strongest single
    # line: type information, location and frame base all at once.
    grep -q 'pt = {x = 3, y = 4}' "$out/gdb.txt" || {
        echo "FAIL: gdb did not print the struct local as {x = 3, y = 4}"
        grep -i 'pt' "$out/gdb.txt" | head -3 | sed 's/^/     | /'; fail=1; }
    [ "$fail" -eq 0 ] &&
        echo "  gdb breaks by source line, reads the arguments, and prints" &&
        echo "  a struct local as {x = 3, y = 4}"
else
    echo "  SKIP the gdb session: no gdb with riscv:rv32, or no $QEMU"
fi

# The image still RUNS -- carrying DWARF must not disturb the loadable
# part, and a non-allocated section landing inside a segment would.
if command -v "$QEMU" > /dev/null 2>&1; then
    got=$(timeout "${EMBCC_QEMU_TIMEOUT:-20}" "$QEMU" -M virt -bios none \
              -nographic -kernel "$out/prog.elf" 2>/dev/null |
          tr -d '\n' | sed 's/==END==.*//')
    [ "$got" = "42 " ] || { echo "FAIL: the image printed '$got', wanted '42 '"
                            fail=1; }
    [ "$fail" -eq 0 ] && echo "  and the image still runs (42)"
fi

[ "$fail" -eq 0 ] || exit 1
