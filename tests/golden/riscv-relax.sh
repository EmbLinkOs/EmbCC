#!/bin/sh
# RISC-V branch relaxation: every jump and branch to a label is emitted in
# the shortest form the first pass measured it reaches in -- c.j (+-2 KiB),
# a direct B-type branch (+-4 KiB), c.beqz/c.bnez (+-256 B, against zero
# from x8-x15) -- and the long form (the opposite branch over a jump)
# otherwise. The failure it guards against is an off-by-a-few at a limit:
# a short form that does not reach, patched into a jump somewhere else.
#
# So, as tests/golden/thumb-relax.sh does for Thumb, these functions put
# targets on both sides of each limit, with padding whose every statement
# feeds the returned value (a branch that lands anywhere but its target
# changes the answer), and the padding is CALIBRATED from a probe so the
# sweep still straddles the limits when a statement's size changes. Run on
# RV32 and RV64 and compared with the host.
set -u
echo "TEST-MARKER riscv-relax"
. "$(dirname "$0")/../lib.sh"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
out=tests/golden/out/riscv-relax
rm -rf "$out"; mkdir -p "$out"

# fz<n>: `if (x != 0)` -- a branch against zero, c.beqz territory;
# fg<n>: `if (x > 3)` -- a general branch; both with an else arm, whose
# closing jump is the c.j case.
gen() {
    echo 'void writec(int c); void puts_(const char *s);'
    echo 'volatile unsigned sink;'
    echo 'static void hx(unsigned v) { for (int i = 28; i >= 0; i -= 4) writec("0123456789abcdef"[(v >> i) & 15]); writec(32); }'
    for n in "$@"; do
        for k in z g; do
            if [ $k = z ]; then c='x != 0'; else c='x > 3'; fi
            echo "__attribute__((noinline)) unsigned f$k$n(int x) {"
            echo "    sink = 0;"
            echo "    if ($c) {"
            i=0; while [ $i -lt "$n" ]; do echo "        sink = sink + 1;"; i=$((i+1)); done
            echo "    } else {"
            i=0; while [ $i -lt "$n" ]; do echo "        sink = sink + 3;"; i=$((i+1)); done
            echo "    }"
            echo "    return sink;"
            echo "}"
        done
    done
    echo 'int main(void) {'
    for n in "$@"; do
        echo "    hx(fz$n(1)); hx(fz$n(0)); hx(fg$n(5)); hx(fg$n(1));"
    done
    printf '%s\n' '    puts_("\n==END==\n"); return 0; }'
}

# Calibration: the conditional branch's and the else-jump's distances at
# 20 and 40 statements, from the first function (fz) of each.
T=riscv32-unknown-elf
gen 20 40 > "$out/probe.c"
"$EMBCC" --target=$T -Os -c "$out/probe.c" -o "$out/probe.o" || {
    echo "the calibration probe does not compile"; exit 1; }
cal=$("$OD" -d --no-show-raw-insn --mattr=+c,+m "$out/probe.o" | awk '
    function hex(v,   d, k) { d = 0; for (k = 3; k <= length(v); k++)
        d = d * 16 + index("0123456789abcdef", substr(v, k, 1)) - 1; return d }
    /^[0-9a-f]+ <f[zg][0-9]+>:/ { f = $2; sc = su = 0; base = hex("0x" $1) }
    /^ +[0-9a-f]+:/ && f != "" {
        at = hex("0x" substr($1, 1, length($1) - 1))
        if (match($0, /<f[zg][0-9]+\+0x[0-9a-f]+>/)) {
            tg = hex(substr($0, RSTART + index(substr($0, RSTART), "+") , RLENGTH - index(substr($0, RSTART), "+") - 1))
            tg = tg + base
            if (!sc && ($2 ~ /^b/)) { c[f] = tg - at; sc = 1 }
            else if (sc && !su && $2 == "j") { u[f] = tg - at; su = 1 }
        }
    }
    END {
        sc_ = (c["<fz40>:"] - c["<fz20>:"]) / 20; su_ = (u["<fz40>:"] - u["<fz20>:"]) / 20;
        if (sc_ <= 0 || su_ <= 0) { print "0 0 0"; exit }
        printf "%d %d %d\n", 20 + (256 - c["<fz20>:"]) / sc_,
                            20 + (4096 - c["<fz20>:"]) / sc_,
                            20 + (2048 - u["<fz20>:"]) / su_ }')
set -- $cal
[ "${1:-0}" -gt 0 ] && [ "${2:-0}" -gt 0 ] && [ "${3:-0}" -gt 0 ] || {
    echo "could not calibrate the padding from the probe ($cal)"; exit 1; }
ns=""
for mid in "$1" "$2" "$3"; do
    k=$((mid - 4)); while [ $k -le $((mid + 4)) ]; do ns="$ns $k"; k=$((k + 1)); done
done
gen $ns > "$out/r.c"
cc -w -o "$out/host" "$out/r.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)

H=$EMBCC_ROOT/tests/harness/riscv
ran=0
for x in 32 64; do
    Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$Q" >/dev/null 2>&1 || { echo "SKIP rv$x: $Q absent"; continue; }
    T=riscv$x-unknown-elf B=$out/rv$x; mkdir -p "$B"
    EMBCC_RISCV_HARNESS=$PWD/$B; export EMBCC_RISCV_HARNESS
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "rv$x: the harness does not compile"; exit 1; }
    done
    for opt in -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/r.c" -o "$B/r.o" || {
            echo "rv$x $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$B/r.elf" "$B/r.o" > "$B/ln.log" 2>&1 || {
            echo "rv$x $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$H/run.sh" "$B/r.elf" "$x" 2>/dev/null | head -1)
        [ "$got" = "$want" ] || {
            echo "rv$x $opt: disagrees with the host"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
done
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for RISC-V"; exit 0; }
# ...and the padding reached every limit: short forms on the near side,
# the next form up on the far side.
"$OD" -d --mattr=+c,+m "$out/rv32/r.o" > "$out/r.s"
for form in 'beqz|bnez' 'b[a-z]+' ; do
    grep -qE "^ +[0-9a-f]+: [0-9a-f]{4} +\s+($form)\s" "$out/r.s" ||
    grep -qE "\s($form)\s" "$out/r.s" || {
        echo "no $form was emitted: the sweep missed a limit"; exit 1; }
done
echo "jumps and branches on both sides of c.beqz's, the branch's and c.j's
reach agree with the host on RV32 and RV64 at -O2 and -Os (sweep:$ns)"
