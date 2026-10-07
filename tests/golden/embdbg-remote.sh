#!/bin/sh
# EmbDBG debugging a RUNNING target, on the two embedded machines.
#
# Everything else in the embdbg family reads a file. This starts QEMU
# with its gdb stub, connects EmbDBG's own remote-protocol client to it,
# sets a breakpoint by FUNCTION NAME, continues, and requires the target
# to stop inside that function with the arguments the caller passed.
#
# That last part is the test. Stopping somewhere is easy; stopping at the
# right address and reading the right registers out of the `g` packet is
# what a wrong register-table offset, an unexpanded run-length encoding
# or a Thumb bit left on a symbol each break. The check is a value the
# test computes itself -- compute(0, 2) is called first, so the two
# argument registers must hold 0 and 2 -- rather than whatever the stub
# happened to say.
#
# Both widths of RISC-V and ARMv7-M, because the register layout is
# per-architecture and an ELF32 image exercises a different reader in
# EmbDBG than an ELF64 one.
set -u
echo "TEST-MARKER embdbg-remote"
. "$(dirname "$0")/../lib.sh"

EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built (make embdbg)"; exit 1; }

out=tests/golden/out/embdbg-remote
rm -rf "$out"; mkdir -p "$out"

# A program with a function whose first two arguments are known at the
# first call, and a loop so `continue` has somewhere to go. The function
# is STATIC: a local symbol, which embld once left out of the image, so
# a debugger had no name for most of a firmware's code.
cat > "$out/fw.c" <<'CEOF'
void puts_(const char *s); void putn(long v);
static int acc;
static int compute(int a, int b) { int t = a * b; acc += t; return t + 1; }
int main(void)
{
    for (int i = 0; i < 3; i++) putn(compute(i, i + 2));
    puts_("\n==END==\n");
    return 0;
}
CEOF

# A port per architecture, high enough to be free and fixed so a failure
# is reproducible. Chosen from the pid so two runs of the suite in
# parallel do not collide.
base=$(( 24000 + ($$ % 900) * 4 ))

ran=0
try_one() {                 # try_one TAG TRIPLE QEMU MACHINE-ARGS HARNESS ARG1 ARG2
    tag=$1; triple=$2; qemu=$3; margs=$4; harness=$5; want1=$6; want2=$7
    command -v "$qemu" >/dev/null 2>&1 || { echo "SKIP $tag: $qemu absent"; return 0; }

    d="$out/$tag"; mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target="$triple" -c "tests/harness/$harness/$f.c" \
                 -o "$d/$f.o" || { echo "$tag: the harness does not compile"; return 1; }
    done
    "$EMBCC" --target="$triple" -O0 -c "$out/fw.c" -o "$d/fw.o" ||
        { echo "$tag: the program does not compile"; return 1; }
    env "EMBCC_$(echo "$harness" | tr a-z A-Z)_HARNESS=$PWD/$d" \
        sh "tests/harness/$harness/link.sh" "$d/fw.elf" "$d/fw.o" ||
        { echo "$tag: embld could not link"; return 1; }

    # The symbol table embld keeps in the executable -- its locals too --
    # is what makes `break compute` resolvable at all; check it before
    # blaming the protocol for a breakpoint that cannot be placed.
    "$EMBDBG" "$d/fw.elf" funcs > "$d/funcs.txt" 2>&1
    grep -q '^compute ' "$d/funcs.txt" || {
        echo "$tag: the linked image has no 'compute' symbol:"
        cat "$d/funcs.txt"; return 1; }

    port=$((base + ran))
    ran=$((ran + 1))
    # -S: stop before the first instruction, so the breakpoint is in
    # place before anything runs and the test does not race the target.
    $qemu $margs -kernel "$d/fw.elf" -S -gdb "tcp::$port" \
        > "$d/qemu.txt" 2>&1 &
    qpid=$!

    # RETRY THE REAL SESSION rather than probing the port first. A probe
    # that connects and disconnects DETACHES, and detaching resumes the
    # target -- so the program ran to completion and the session that
    # followed found nothing to stop. Retrying costs nothing when the
    # stub is already up, which it almost always is.
    i=0
    while [ $i -lt 50 ]; do
        printf 'break compute\ncontinue\nregs\nbt\nquit\n' |
            "$EMBDBG" "$d/fw.elf" remote "localhost:$port" \
            > "$d/session.txt" 2>&1
        grep -q '^connected to' "$d/session.txt" && break
        i=$((i + 1))
        sleep 0.1
    done
    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null

    grep -q "breakpoint 1 at .*compute" "$d/session.txt" || {
        echo "$tag: could not set a breakpoint on compute:"
        sed -n '1,12p' "$d/session.txt"; return 1; }
    # The stop must be INSIDE compute, not merely somewhere.
    grep -q "^stopped at .*compute" "$d/session.txt" || {
        echo "$tag: the target did not stop in compute:"
        sed -n '1,12p' "$d/session.txt"; return 1; }
    # And the arguments must be the ones main passed on the first call.
    # This is what pins the register table: a wrong offset for the
    # argument registers reads someone else's value here.
    grep -qE "(^| )$want1 +0*0( |$)" "$d/session.txt" || {
        echo "$tag: the first argument register does not hold 0:"
        sed -n '/^stopped/,/^  #0/p' "$d/session.txt"; return 1; }
    grep -qE "(^| )$want2 +0*2( |$)" "$d/session.txt" || {
        echo "$tag: the second argument register does not hold 2:"
        sed -n '/^stopped/,/^  #0/p' "$d/session.txt"; return 1; }
    # Frame 1 comes from the return-address register and must be main.
    grep -q "#1 .*main" "$d/session.txt" || {
        echo "$tag: the caller was not identified as main:"
        sed -n '/#0/,/#1/p' "$d/session.txt"; return 1; }
    echo "$tag: stopped in compute(0, 2), called from main"
    return 0
}

try_one riscv64 riscv64-unknown-elf qemu-system-riscv64 \
        "-M virt -bios none -nographic -m 8" riscv a0 a1 || exit 1
try_one riscv32 riscv32-unknown-elf qemu-system-riscv32 \
        "-M virt -bios none -nographic -m 8" riscv a0 a1 || exit 1
try_one thumb thumbv7m-none-eabi qemu-system-arm \
        "-M lm3s6965evb -cpu cortex-m3 -nographic" thumb r0 r1 || exit 1

# ---- AVR (ATmega328P on QEMU's arduino-uno), by SOURCE LINE ------------
#
# The others above break on a function and read argument registers. This
# one goes the whole way on the 8-bit part, built with -g: its own register
# layout (r0-r31 one byte each, SREG, a two-byte SP and a four-byte PC that
# is a BYTE address), a register WRITE, a breakpoint by FILE:LINE out of
# the DWARF, a local and a global read by name, and a step to the next
# line. Each value is one the test can predict:
#
#   * at compute's entry the arguments of the first call, 3 and 4, are in
#     r24 and r22 and the pc is compute's own address;
#   * `set r24 7` changes the first argument before the prologue stores
#     it, so at line 6 t must be 7*4+5 = 33 -- and the program, run on to
#     its end, prints 34 where it would have printed 18;
#   * acc is 100 before line 6 and 133 after one step.
#
# A local is read at Y + its DW_OP_fbreg, Y being r28:r29 -- the PAIR --
# in DATA space, which QEMU's stub (like GDB) puts at 0x800000 up; a
# global's DW_OP_addr carries that offset already. Reading r28 alone, or
# flash instead of SRAM, prints a wrong number here rather than nothing.
#
# QEMU runs through qrun.sh, whose limits hold even if this shell dies:
# --until stops it as soon as the guest, let go when EmbDBG detaches,
# prints its end marker.
try_avr() {
    qemu=${EMBCC_QEMU_AVR:-qemu-system-avr}
    command -v "$qemu" >/dev/null 2>&1 || { echo "SKIP avr: $qemu absent"; return 0; }
    d="$out/avr"; mkdir -p "$d"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$d/boot.o" &&
    "$EMBCC" --target=avr -c tests/harness/avr/io.c -o "$d/io.o" &&
    "$EMBCC" --target=avr -c lib/rt/avr.c -o "$d/rt.o" ||
        { echo "avr: the harness does not compile"; return 1; }
    cat > "$d/fwa.c" <<'CEOF'
void puts_(const char *s); void putn(long v);
int acc = 100;
static int compute(int a, int b)
{
    int t = a * b + 5;
    acc += t;
    return t + 1;
}
int main(void)
{
    for (int i = 0; i < 3; i++) putn(compute(i + 3, i + 4));
    puts_("\n==END==\n");
    return 0;
}
CEOF
    "$EMBCC" --target=avr -g -O0 -c "$d/fwa.c" -o "$d/fwa.o" ||
        { echo "avr: the program does not compile with -g"; return 1; }
    EMBCC_AVR_HARNESS="$PWD/$d" sh tests/harness/avr/link.sh "$d/fwa.elf" \
        "$d/fwa.o" > "$d/ld.txt" 2>&1 ||
        { echo "avr: embld could not link"; cat "$d/ld.txt"; return 1; }
    entry=$("$EMBDBG" "$d/fwa.elf" funcs 2>/dev/null |
            awk '$1 == "compute" { print $2 }')
    [ -n "$entry" ] || { echo "avr: no 'compute' in the image"; return 1; }
    epad=$(printf '%08x' "$entry")

    # The global's DW_OP_addr is its data-space address: 0x800000 + where
    # the linker put it in SRAM.
    if command -v llvm-dwarfdump >/dev/null 2>&1 && command -v llvm-nm >/dev/null 2>&1; then
        sym=$(llvm-nm "$d/fwa.elf" | awk '$3 == "acc" { print $1 }')
        dw=$(llvm-dwarfdump --debug-info "$d/fwa.elf" |
             grep -A3 'DW_AT_name	("acc")' | sed -n 's/.*DW_OP_addr 0x\([0-9a-f]*\).*/\1/p')
        [ -n "$sym" ] && [ -n "$dw" ] &&
            [ $((0x$dw)) -eq $((0x800000 + 0x$sym)) ] || {
            echo "avr: acc's DW_OP_addr is 0x$dw; it is at 0x$sym in SRAM, so 0x80$sym"
            return 1; }
    fi

    port=$((base + ran))
    ran=$((ran + 1))
    sh tests/harness/qrun.sh 60 --until '==END==' "$qemu" -M uno -nographic \
        -bios "$d/fwa.elf" -S -gdb "tcp::$port" > "$d/qemu.txt" 2>&1 &
    qpid=$!
    i=0
    while [ $i -lt 50 ]; do
        printf '%s\n' "break *$entry" "break fwa.c:6" continue regs \
            "set r24 7" regs continue "print t" "print a" "print b" \
            "print acc" step "print acc" quit |
            "$EMBDBG" "$d/fwa.elf" remote "localhost:$port" \
            > "$d/session.txt" 2>&1
        grep -q '^connected to' "$d/session.txt" && break
        i=$((i + 1))
        sleep 0.1
    done
    wait "$qpid" 2>/dev/null
    pkill -f "tcp::$port" 2>/dev/null

    s="$d/session.txt"
    show() { sed -n '1,40p' "$s" | sed 's/^/     | /'; }
    grep -q "^stopped at $entry .*compute" "$s" || {
        echo "avr: did not stop at compute's entry ($entry):"; show; return 1; }
    # the first `regs`: the first call's arguments, and the pc where it
    # stopped -- the four-byte byte address, 35 bytes into the packet
    grep -qE "^r24 +03  r25 +00 " "$s" && grep -qE "r22 +04  r23 +00( |$)" "$s" || {
        echo "avr: r24:r25 / r22:r23 do not hold compute's arguments 3 and 4:"; show; return 1; }
    grep -qE "pc +$epad( |$)" "$s" || {
        echo "avr: the pc does not read $epad, the breakpoint's address:"; show; return 1; }
    sp=$(sed -n 's/.*sp *\([0-9a-f][0-9a-f][0-9a-f][0-9a-f]\)  pc.*/\1/p' "$s" | head -1)
    [ -n "$sp" ] && [ $((0x$sp)) -ge 256 ] && [ $((0x$sp)) -le 2303 ] || {
        echo "avr: sp reads '$sp', not an address in the ATmega328P's SRAM:"; show; return 1; }
    grep -qE "^sreg +[0-9a-f][0-9a-f]  sp " "$s" || {
        echo "avr: no SREG beside SP:"; show; return 1; }
    # the write, as the stub reads it back
    grep -qE "^r24 +07  r25 +00 " "$s" || {
        echo "avr: 'set r24 7' did not reach the target:"; show; return 1; }
    grep -q "^stopped at .*compute.*fwa.c:6" "$s" || {
        echo "avr: no stop at fwa.c:6, the breakpoint by source line:"; show; return 1; }
    for want in "t = 33" "a = 7" "b = 4" "acc = 100"; do
        grep -qx "$want" "$s" || {
            echo "avr: wanted '$want' at line 6:"; sed -n '/fwa.c:6/,$p' "$s" | head -24 |
                sed 's/^/     | /'; return 1; }
    done
    grep -q "^stopped at .*compute.*fwa.c:7" "$s" || {
        echo "avr: 'step' did not reach line 7:"; show; return 1; }
    sed -n '/fwa.c:7/,$p' "$s" | grep -qx "acc = 133" || {
        echo "avr: acc is not 133 after the step:"; sed -n '/fwa.c:7/,$p' "$s" | sed 's/^/     | /'
        return 1; }
    # let go, the program finished -- with the argument the debugger wrote
    grep -q "34 26 36" "$d/qemu.txt" || {
        echo "avr: the program printed '$(head -c 80 "$d/qemu.txt")', not 34 26 36"
        return 1; }
    echo "avr: registers, a write, a break at fwa.c:6, t/a/b/acc by name, a step to line 7"

    # The same image under a real gdb, where one is installed with AVR in
    # it: an independent reading of the same DWARF (breg28, DW_OP_addr).
    if command -v gdb >/dev/null 2>&1 &&
       gdb -batch -nx -ex 'set architecture avr' >/dev/null 2>&1; then
        port=$((base + ran))
        ran=$((ran + 1))
        sh tests/harness/qrun.sh 60 --until '==END==' "$qemu" -M uno -nographic \
            -bios "$d/fwa.elf" -S -gdb "tcp::$port" > "$d/qemu2.txt" 2>&1 &
        qpid=$!
        sleep 1
        gdb -batch -nx -q "$d/fwa.elf" -ex 'set architecture avr' \
            -ex "target remote :$port" -ex 'break fwa.c:6' -ex continue \
            -ex 'info args' -ex 'print t' -ex 'print acc' -ex next \
            -ex 'print acc' > "$d/gdb.txt" 2>&1
        wait "$qpid" 2>/dev/null
        pkill -f "tcp::$port" 2>/dev/null
        grep -q 'a = 3' "$d/gdb.txt" && grep -q 'b = 4' "$d/gdb.txt" &&
        grep -q '= 17$' "$d/gdb.txt" && grep -q '= 100$' "$d/gdb.txt" &&
        grep -q '= 117$' "$d/gdb.txt" || {
            echo "avr: gdb reads the same DWARF differently:"
            grep -vi warning "$d/gdb.txt" | head -12 | sed 's/^/     | /'; return 1; }
        echo "avr: gdb agrees: a = 3, b = 4, t = 17, acc 100 then 117"
    fi
    return 0
}
try_avr || exit 1

[ "$ran" -gt 0 ] || { echo "skipped: no QEMU for any embedded target"; exit 0; }
echo "EmbDBG drives a live target through a gdb stub on $ran machine(s)"
