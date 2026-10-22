#!/bin/sh
# RISC-V jumps and branches past jal's +-1 MiB, in a function bigger than
# that: GCC's torture compile/limits-caselabels.c (a 30000-case switch)
# and 20001226-1.c stopped the RV32 backend at -O0 with "internal error:
# riscv: jump displacement: 4432840 does not fit in 21 signed bits".
# Such a function is made again with every jump in its far form, `auipc
# t6; jalr x0, t6`, and every branch the opposite branch over one --
# shortened, as ever, where a shorter form reaches (-O1 and up).
#
# big() below is two arms of tens of thousands of statements (a macro,
# so the source stays small): the branch into the else arm, the jump over
# it and the loop's branch back each span more than 1 MiB. Every
# statement feeds the result, so a jump that lands anywhere but its
# target changes it. At -O0 a 32-bit statement is 32 bytes; at -O2 it is
# 14 and a 64-bit one at RV32 56, which is what keeps that compile to
# seconds -- so -O2 runs at RV32, -O0 at both widths, each compared with
# the host, and each object must have the far form.
set -u
echo "TEST-MARKER riscv-far-jump"
. "$(dirname "$0")/../lib.sh"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/riscv-far-jump
rm -rf "$out"; mkdir -p "$out"

cat > "$out/r.c" <<'EOT'
void writec(int c); void puts_(const char *s);
#ifdef O2
volatile unsigned long long sink;       /* 28672 statements an arm */
#define ARM(s) C(C(s)) C(C(s)) C(C(s)) C(C(s)) C(C(s)) C(C(s)) C(C(s))
#else
volatile unsigned sink;                 /* 40960 */
#define ARM(s) C(C(B(s))) C(C(s)) C(C(s))
#endif
static void hx(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(32);
}
#define A1 sink = sink * 7u + 1u;
#define A3 sink = sink * 5u + 3u;
#define B(s) s s s s s s s s
#define C(s) B(B(s))
__attribute__((noinline)) unsigned big(int x)
{
    int i = 0;
    sink = 0;
again:
    if (x > 3) {
        ARM(A1)
    } else {
        ARM(A3)
    }
    x -= 3;
    if (++i < 3)
        goto again;
    return (unsigned)sink ^ (unsigned)(sink >> 16 >> 16);
}
int main(void)
{
    hx(big(5)); hx(big(1)); hx(big(7));
    puts_("\n==END==\n");
    return 0;
}
EOT
for opt in -O0 -O2; do
    cc -w -D${opt#-} -o "$out/host$opt" "$out/r.c" \
        "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null || {
        echo "the host reference does not build"; exit 1; }
done

H=$EMBCC_ROOT/tests/harness/riscv
ran=0
for cfg in "32 -O0" "32 -O2" "64 -O0"; do
    set -- $cfg
    x=$1 opt=$2
    Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$Q" >/dev/null 2>&1 || { echo "SKIP rv$x: $Q absent"; continue; }
    T=riscv$x-unknown-elf B=$out/rv$x; mkdir -p "$B"
    EMBCC_RISCV_HARNESS=$PWD/$B; export EMBCC_RISCV_HARNESS
    for f in boot io; do
        [ -f "$B/$f.o" ] || "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "rv$x: the harness does not compile"; exit 1; }
    done
    obj=$B/r$opt.o
    perl -e 'alarm 120; exec @ARGV' \
        "$EMBCC" --target=$T $opt -D${opt#-} -c "$out/r.c" -o "$obj" || {
        echo "rv$x $opt: does not compile, or not in 120 s"; exit 1; }
    sh "$H/link.sh" "$B/r.elf" "$obj" > "$B/ln.log" 2>&1 || {
        echo "rv$x $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
    want=$("$out/host$opt" | head -1)
    got=$(EMBCC_QEMU_TIMEOUT=60 sh "$H/run.sh" "$B/r.elf" "$x" 2>/dev/null |
          head -1)
    [ "$got" = "$want" ] || {
        echo "rv$x $opt: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
    # ...and the far form is what reached: a jalr x0 through t6
    if command -v "$OD" >/dev/null 2>&1; then
        n=$("$OD" -d --no-show-raw-insn --mattr=+c,+m "$obj" |
            grep -cE '(jr|jalr)[[:space:]].*\(t6\)')
        [ "$n" -gt 0 ] || {
            echo "rv$x $opt: no jump through t6 -- the far form was not used"
            exit 1; }
    fi
    ran=$((ran + 1))
done
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for RISC-V"; exit 0; }
echo "riscv-far-jump: jumps and branches over 1 MiB agree with the host ($ran runs: RV32 -O0 and -O2, RV64 -O0)"
