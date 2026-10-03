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

[ "$ran" -gt 0 ] || { echo "skipped: no QEMU for any embedded target"; exit 0; }
echo "EmbDBG drives a live target through a gdb stub on $ran machine(s)"
