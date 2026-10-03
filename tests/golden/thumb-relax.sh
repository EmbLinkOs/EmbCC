#!/bin/sh
# Branch relaxation: a branch the first pass measured within reach is
# emitted in its 16-bit form (b<c> reaches -256..+254 bytes, b
# -2048..+2046), everything else 32-bit. The failure it guards against is
# an off-by-a-few at those limits -- a short branch that does not reach,
# patched into a jump to somewhere else -- so the programs here put
# branch targets on both sides of every limit, forward and backward, by
# padding with a run of volatile stores whose length is swept a few
# instructions at a time. Each is run on the Cortex-M3 and compared with
# the host. (Only the if/else arms get the large paddings: a loop body of
# 500 stores in every function made an image bigger than the part's
# flash, and it simply did not run.)
set -u
echo "TEST-MARKER thumb-relax"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }

T=thumbv7m-none-eabi
H=$EMBCC_ROOT/tests/harness/thumb
out=tests/golden/out/thumb-relax
rm -rf "$out"; mkdir -p "$out"
EMBCC_THUMB_HARNESS=$PWD/$out; export EMBCC_THUMB_HARNESS
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done

# One function per padding length N: a forward conditional branch over N
# statements, a forward unconditional one (the jump at the end of the `if`
# arm) over N more, and a backward conditional one around a loop. Every
# statement ADDS to a volatile counter that the function returns -- a
# different amount in each arm -- so a branch that lands anywhere but its
# target shows in the answer. (Plain stores did not: the optimizer
# computes the `if`'s own result as a select, and a misplaced jump then
# changed only which stores happened, which nothing printed.)
gen() {
    echo 'void writec(int c); void puts_(const char *s);'
    echo 'volatile unsigned sink;'
    echo 'static void hx(unsigned v) { for (int i = 28; i >= 0; i -= 4) writec("0123456789abcdef"[(v >> i) & 15]); writec(32); }'
    for n in "$@"; do
        echo "__attribute__((noinline)) unsigned f$n(int x, int k) {"
        echo "    sink = 0;"
        echo "    if (x > 3) {"
        i=0; while [ $i -lt "$n" ]; do echo "        sink = sink + 1;"; i=$((i+1)); done
        echo "    } else {"
        i=0; while [ $i -lt "$n" ]; do echo "        sink = sink + 3;"; i=$((i+1)); done
        echo "    }"
        echo "    do {"
        # the loop's backward branch only needs the conditional limit
        m=$n; [ "$m" -gt 60 ] && m=4
        i=0; while [ $i -lt "$m" ]; do echo "        sink = sink + 5;"; i=$((i+1)); done
        echo "    } while (--k > 0);"
        echo "    return sink;"
        echo "}"
    done
    echo 'int main(void) {'
    for n in "$@"; do
        echo "    hx(f$n(5, 3)); hx(f$n(1, 2));"
    done
    printf '%s\n' '    puts_("\n==END==\n"); return 0; }'
}
# Where the limits fall depends on how big a statement compiles to, which
# every size improvement changes -- a hard-coded sweep drifted off both
# limits the day `add` got a 16-bit form. So it is CALIBRATED: compile a
# probe with 20 and 40 statements, read the conditional (the `if`) and the
# unconditional (the jump over `else`) distances from the object code, and
# centre each sweep on the padding that puts its branch at the limit.
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
gen 20 40 > "$out/probe.c"
"$EMBCC" --target=$T -Os -c "$out/probe.c" -o "$out/probe.o" || {
    echo "the calibration probe does not compile"; exit 1; }
cal=$("$OD" -d --no-show-raw-insn --triple=thumbv7m "$out/probe.o" | awk '
    function hex(v,   d, k) { d = 0; for (k = 3; k <= length(v); k++)
        d = d * 16 + index("0123456789abcdef", substr(v, k, 1)) - 1; return d }
    /^[0-9a-f]+ <f[0-9]+>:/ { f = $2; seen_c = seen_u = 0 }
    /\t(ble|bgt|blt|bge)(\.w)?\t/ && !seen_c && f != "" {
        match($0, /imm = #0x[0-9a-f]+/); c[f] = hex(substr($0, RSTART + 7, RLENGTH - 7)); seen_c = 1 }
    /\tb(\.w)?\t/ && seen_c && !seen_u {
        match($0, /imm = #0x[0-9a-f]+/); if (RSTART) { u[f] = hex(substr($0, RSTART + 7, RLENGTH - 7)); seen_u = 1 } }
    END {
        sc = (c["<f40>:"] - c["<f20>:"]) / 20; su = (u["<f40>:"] - u["<f20>:"]) / 20;
        if (sc <= 0 || su <= 0) { print "0 0"; exit }
        printf "%d %d\n", 20 + (256 - c["<f20>:"]) / sc, 20 + (2048 - u["<f20>:"]) / su }')
nc=${cal% *}; nu=${cal#* }
[ "$nc" -gt 0 ] && [ "$nu" -gt 0 ] || {
    echo "could not calibrate the padding from the probe"; exit 1; }
ns=20
k=$((nc - 5)); while [ $k -le $((nc + 5)) ]; do ns="$ns $k"; k=$((k + 1)); done
k=$((nu - 5)); while [ $k -le $((nu + 5)) ]; do ns="$ns $k"; k=$((k + 1)); done
gen $ns > "$out/r.c"
cc -w -o "$out/host" "$out/r.c" "$H/hostio.c" 2>/dev/null ||
    { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
nrel=0
# -O2 and -Os: relaxation is part of the optimising build only.
for opt in -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$out/r.c" -o "$out/r.o" || {
        echo "$opt: does not compile"; exit 1; }
    sh "$H/link.sh" "$out/r.elf" "$out/r.o" > "$out/ln.log" 2>&1 || {
        echo "$opt: does not link"; head -3 "$out/ln.log"; exit 1; }
    got=$(sh "$H/run.sh" "$out/r.elf" 2>&1 | head -1)
    [ "$got" = "$want" ] || {
        echo "$opt: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
    OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
    if command -v "$OD" >/dev/null 2>&1; then
        n16=$("$OD" -d --triple=thumbv7m "$out/r.o" |
              grep -cE '^ +[0-9a-f]+: +(d[0-9a-d]|e[0-7])[0-9a-f]{2}[[:space:]]+b' || true)
        nrel=$((nrel + n16))
    fi
done
[ "$nrel" -gt 0 ] || { echo "no branch was relaxed: the test proves nothing"; exit 1; }
# ...and the sweep must still reach each side of each limit, or the
# programs agree with the host without having tested the boundary.
if command -v "$OD" >/dev/null 2>&1; then
    "$OD" -d --no-show-raw-insn --triple=thumbv7m "$out/r.o" |
        awk '/\tb[a-z]*(\.w)?\t/ { match($0, /imm = #-?0x[0-9a-f]+/);
             v = substr($0, RSTART + 7, RLENGTH - 7); neg = v ~ /^-/;
             sub(/^-/, "", v); d = 0;
             for (k = 3; k <= length(v); k++)
                 d = d * 16 + index("0123456789abcdef", substr(v, k, 1)) - 1;
             if (neg) d = -d;
             u = ($2 == "b" || $2 == "b.w");
             if (!u && d >= 236 && d <= 254) a = 1;  if (!u && d > 254 && d < 280) b = 1;
             if (u && d >= 2020 && d <= 2046) c = 1; if (u && d > 2046 && d < 2080) e = 1 }
             END { exit !(a && b && c && e) }' || {
        echo "the padding no longer straddles the 16-bit limits"; exit 1; }
fi
echo "branches on both sides of the 16-bit limits, forward and backward,
conditional and not, agree with the host at -O2 and -Os ($nrel short)"

# cbz has a LOWER limit too: it branches forward from pc, never to the
# instruction right after it. A branch around nothing -- `l0 = l0`, a
# move from a register to itself -- has its label there. The first pass
# measured it as `cmp; b<c>.w` with the label 2 bytes past the cmp's
# reach, in range, and the second pass's cbz then could not encode -2:
# an internal error at -O2 and -Os on both cores (fuzz seed 3085).
cat > "$out/next.c" <<'EOF'
volatile double vd = 2.0;
unsigned f(unsigned l0, double d)
{
    if (d == vd)
        l0 = (unsigned)l0;
    return l0 * 3u;
}
EOF
for tg in thumbv7m-none-eabi thumbv7em-none-eabi; do
    for opt in -O1 -O2 -Os; do
        "$EMBCC" --target=$tg $opt -c "$out/next.c" -o "$out/next.o" || {
            echo "$tg $opt: a branch to the next instruction does not compile"
            exit 1; }
    done
done
echo "a conditional branch whose label is the next instruction compiles"
