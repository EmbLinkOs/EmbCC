#!/bin/sh
# embsim-mips: EmbSim's MIPS32 core (tools/embsim/mips.c), refereed by
# QEMU's malta board with a 24Kc -- the machine tests/harness/mips runs
# EmbCC's mipsel output on.
#
#  1. The exec corpus at -O0, -O2 and -Os: every tests/exec program built
#     with lib/libc for mipsel and linked with the harness, run on
#     qemu-system-mipsel -M malta and on EmbSim's malta. The output, with
#     its ==EXIT n== sentinel, must be the same. A program the harness
#     cannot build, or QEMU cannot finish, is not applicable.
#  2. The ends of a run (no QEMU needed): the board's reset (SOFTRES), an
#     exception whose vector cannot be fetched, sdbbp with no debugger, a
#     loop nothing can interrupt, a wait nothing can wake, --until and
#     --max-insns.
#  3. Instruction edges (tests/golden/embsim/mips-isa.c, built by clang):
#     lwl/lwr/swl/swr at each offset, ext and ins, the byte shuffles and
#     rotates, clz and clo, the multiplies, divides and accumulates (and
#     the divides QEMU defines where the architecture does not), the
#     likely and linking branches, the exceptions and their codes (one in
#     a delay slot), ll and sc, rdhwr, the TLB and its refill vector, the
#     core timer's interrupt waking a wait. EmbSim's output must be
#     QEMU's, and the record's (tests/golden/embsim/mips-isa.txt). And
#     the FPU's (tests/golden/embsim/mips-fpu.c) on the pic32mz core,
#     against QEMU's P5600 (FR 1, the 2008 modes): each operation over a
#     table of values in the four rounding modes, the flags, the
#     compares, the moves and loads, the floating-point exception. And
#     the DSP ASE's (tests/golden/embsim/mips-dsp.c), Revision 2, on the
#     pic32mz core against QEMU's 74Kf: each instruction over a table of
#     operands, accumulators and DSPControl states, the masks of rddsp
#     and wrdsp, bposge32, the indexed loads, Status.MX clear.
set -u
echo "TEST-MARKER embsim-mips"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
export EMBCC EMBLD EMBSIM
out=tests/golden/out/embsim-mips
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
have_qemu=0
command -v "$QEMU" >/dev/null 2>&1 && have_qemu=1
qemu_run() {        # qemu_run IMAGE: as tests/harness/mips/run.sh does
    sh tests/harness/qrun.sh 20 "$QEMU" -M malta -cpu 24Kc -m 64 \
        -display none -monitor none -serial null -serial null \
        -serial stdio -no-reboot -kernel "$1" 2>/dev/null
}

# ---- 2. the ends of a run ------------------------------------------------
e=$out/ends; mkdir -p "$e"
cat > "$e/ends.c" <<'EOF'
/* MODE 0: SOFTRES after printing; 1: an exception with EBase where
 * nothing is (0x9e000000): a lockup; 2: sdbbp; 3: a loop with interrupts
 * off; 4: wait with nothing enabled; 5: prints, then loops for ever
 * printing */
#define THR (*(volatile unsigned char *)0xbf000900u)
#define SOFTRES (*(volatile unsigned *)0xbf000500u)
static void puts_(const char *s) { while (*s) THR = (unsigned char)*s++; }
int main(void)
{
    puts_("ready\n");
#if MODE == 0
    SOFTRES = 0x42;
#elif MODE == 1
    unsigned st;
    __asm__ volatile("mtc0 %0, $15, 1" : : "r"(0x9e000000u));
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st & ~((1u << 22) | 6u)));
    __asm__ volatile("syscall");
#elif MODE == 2
    __asm__ volatile("sdbbp 0");
#elif MODE == 3
    for (;;)
        ;
#elif MODE == 4
    __asm__ volatile("wait");
    puts_("woke\n");
#else
    for (volatile int i = 0;; i++)
        if (i == 1000)
            puts_("==MARK==\n");
#endif
    for (;;)
        ;
}
void _start(void) { main(); }
EOF
for m in 0 1 2 3 4 5; do
    "$EMBCC" --target=mipsel-none-elf -O1 -DMODE=$m -c "$e/ends.c" -o "$e/e$m.o" &&
    "$EMBLD" -e _start -Ttext 0x80100000 -Tstack 0x80800000 "$e/e$m.o" \
        -o "$e/e$m.elf" || fail "the end-of-run images do not build"
done
"$EMBSIM" "$e/e0.elf" --board malta --stats --max-insns 1000000 > "$e/0.out" 2>&1; st=$?
[ $st = 0 ] && grep -q ready "$e/0.out" && grep -q 'SOFTRES' "$e/0.out" ||
    { cat "$e/0.out"; fail "SOFTRES does not end the run (status $st)"; }
"$EMBSIM" "$e/e1.elf" --board malta --max-insns 1000000 > "$e/1.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'lockup: the exception vector 0x9e000180' "$e/1.out" ||
    { cat "$e/1.out"; fail "an exception with no vector is not a lockup (status $st)"; }
"$EMBSIM" "$e/e2.elf" --board malta --max-insns 1000000 > "$e/2.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'sdbbp at 0x8010' "$e/2.out" ||
    { cat "$e/2.out"; fail "sdbbp with no debugger does not stop the run (status $st)"; }
"$EMBSIM" "$e/e3.elf" --board malta --stats --max-insns 1000000 > "$e/3.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'nothing can interrupt' "$e/3.out" ||
    { cat "$e/3.out"; fail "an idle loop does not end the run (status $st)"; }
"$EMBSIM" "$e/e4.elf" --board malta --stats --max-insns 1000000 > "$e/4.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'waiting for an interrupt' "$e/4.out" && ! grep -q woke "$e/4.out" ||
    { cat "$e/4.out"; fail "a wait nothing can wake does not end the run (status $st)"; }
"$EMBSIM" "$e/e5.elf" --board malta --until '==MARK==' --max-insns 10000000 > "$e/5.out" 2>&1; st=$?
[ $st = 0 ] && [ "$(grep -c MARK "$e/5.out")" = 1 ] || fail "--until does not stop at the first ==MARK== (status $st)"
"$EMBSIM" "$e/e5.elf" --board malta --max-insns 500 > "$e/6.out" 2>&1; st=$?
[ $st = 4 ] && grep -q '500 instructions' "$e/6.out" || { cat "$e/6.out"; fail "--max-insns 500: status $st"; }
echo "embsim-mips: SOFTRES, a vector that cannot be fetched, sdbbp, an idle loop, a wait nothing wakes, --until and --max-insns end a run as documented"

# ---- 3. instruction edges ----------------------------------------------------
x=$out/isa; mkdir -p "$x"
CLANG=${EMBCC_REF_CLANG:-clang}
if "$CLANG" -print-targets 2>/dev/null | grep -q mipsel; then
    "$CLANG" --target=mipsel-none-elf -march=mips32r2 -mabi=32 -fno-pic \
        -mno-abicalls -G0 -msoft-float -O1 -ffreestanding -fno-builtin \
        -nostdlib -c tests/golden/embsim/mips-isa.c -o "$x/isa.o" 2>"$x/isa.err" ||
        { cat "$x/isa.err"; fail "mips-isa.c does not compile"; }
    "$EMBLD" -e _start -Ttext 0x80100000 -Tstack 0x80800000 "$x/isa.o" \
        -o "$x/isa.elf" || fail "mips-isa.c does not link"
    "$EMBSIM" "$x/isa.elf" --board malta --max-insns 100000000 > "$x/isa.sim" 2>&1; st=$?
    [ $st = 0 ] || { tail -5 "$x/isa.sim"; fail "mips-isa.c: EmbSim exits $st"; }
    cmp -s "$x/isa.sim" tests/golden/embsim/mips-isa.txt || {
        diff tests/golden/embsim/mips-isa.txt "$x/isa.sim" | head -20
        fail "mips-isa.c: EmbSim's output is not mips-isa.txt"; }
    if [ $have_qemu = 1 ]; then
        qemu_run "$x/isa.elf" > "$x/isa.qemu"
        cmp -s "$x/isa.qemu" "$x/isa.sim" || {
            diff "$x/isa.qemu" "$x/isa.sim" | head -20
            fail "mips-isa.c: EmbSim's output is not QEMU's"; }
    fi
    echo "embsim-mips: the instruction edges ($(wc -l < "$x/isa.sim" | tr -d ' ') results) as QEMU and the record have them"
    # the FPU's (the pic32mz core's, a PIC32MZ EF's), against QEMU's P5600:
    # 64-bit registers, the 2008 NaN and abs modes
    "$CLANG" --target=mipsel-none-elf -march=mips32r2 -mabi=32 -mfp64 \
        -mhard-float -fno-pic -mno-abicalls -G0 -O1 -ffreestanding \
        -fno-builtin -nostdlib -c tests/golden/embsim/mips-fpu.c -o "$x/fpu.o" 2>"$x/fpu.err" ||
        { cat "$x/fpu.err"; fail "mips-fpu.c does not compile"; }
    "$EMBLD" -e _start -Ttext 0x80100000 -Tstack 0x80800000 "$x/fpu.o" \
        -o "$x/fpu.elf" || fail "mips-fpu.c does not link"
    "$EMBSIM" "$x/fpu.elf" --board malta --cpu pic32mz --max-insns 100000000 \
        > "$x/fpu.sim" 2>&1; st=$?
    [ $st = 0 ] || { tail -5 "$x/fpu.sim"; fail "mips-fpu.c: EmbSim exits $st"; }
    cmp -s "$x/fpu.sim" tests/golden/embsim/mips-fpu.txt || {
        diff tests/golden/embsim/mips-fpu.txt "$x/fpu.sim" | head -20
        fail "mips-fpu.c: EmbSim's output is not mips-fpu.txt"; }
    if [ $have_qemu = 1 ]; then
        sh tests/harness/qrun.sh 30 "$QEMU" -M malta -cpu P5600 -m 64 \
            -display none -monitor none -serial null -serial null \
            -serial stdio -no-reboot -kernel "$x/fpu.elf" > "$x/fpu.qemu" 2>/dev/null
        cmp -s "$x/fpu.qemu" "$x/fpu.sim" || {
            diff "$x/fpu.qemu" "$x/fpu.sim" | head -20
            fail "mips-fpu.c: EmbSim's output is not QEMU's (P5600)"; }
    fi
    echo "embsim-mips: the FPU's edges ($(wc -l < "$x/fpu.sim" | tr -d ' ') results: every operation in every rounding mode, the flags, compares, moves, the exceptions) as QEMU's P5600 and the record have them"
    # the DSP ASE's (Revision 2, the pic32mz core's), against QEMU's 74Kf
    "$CLANG" --target=mipsel-none-elf -march=mips32r2 -mabi=32 -mdspr2 \
        -fno-pic -mno-abicalls -G0 -msoft-float -O1 -ffreestanding \
        -fno-builtin -nostdlib -c tests/golden/embsim/mips-dsp.c -o "$x/dsp.o" 2>"$x/dsp.err" ||
        { cat "$x/dsp.err"; fail "mips-dsp.c does not compile"; }
    "$EMBLD" -e _start -Ttext 0x80100000 -Tstack 0x80800000 "$x/dsp.o" \
        -o "$x/dsp.elf" || fail "mips-dsp.c does not link"
    "$EMBSIM" "$x/dsp.elf" --board malta --cpu pic32mz --max-insns 500000000 \
        > "$x/dsp.sim" 2>&1; st=$?
    [ $st = 0 ] || { tail -5 "$x/dsp.sim"; fail "mips-dsp.c: EmbSim exits $st"; }
    cmp -s "$x/dsp.sim" tests/golden/embsim/mips-dsp.txt || {
        diff tests/golden/embsim/mips-dsp.txt "$x/dsp.sim" | head -20
        fail "mips-dsp.c: EmbSim's output is not mips-dsp.txt"; }
    if [ $have_qemu = 1 ]; then
        sh tests/harness/qrun.sh 60 "$QEMU" -M malta -cpu 74Kf -m 64 \
            -display none -monitor none -serial null -serial null \
            -serial stdio -no-reboot -kernel "$x/dsp.elf" > "$x/dsp.qemu" 2>/dev/null
        cmp -s "$x/dsp.qemu" "$x/dsp.sim" || {
            diff "$x/dsp.qemu" "$x/dsp.sim" | head -20
            fail "mips-dsp.c: EmbSim's output is not QEMU's (74Kf)"; }
    fi
    echo "embsim-mips: the DSP ASE's edges ($(wc -l < "$x/dsp.sim" | tr -d ' ') results: every instruction over the lanes' extremes, the accumulators, DSPControl's flags, the exceptions) as QEMU's 74Kf and the record have them"
else
    echo "embsim-mips: no clang for mipsel: the instruction-edge programs were not built"
fi

# ---- 1. the exec corpus -----------------------------------------------------
L=$out/lib
mkdir -p "$L"
{ sh tools/build-rt.sh mipsel-none-elf "$L" && sh tools/build-libc.sh mipsel-none-elf "$L"; } \
    > "$L/build.log" 2>&1 || fail "lib/rt or lib/libc does not build for mipsel"
"$EMBCC" --target=mipsel-none-elf -DHARNESS_LIBC -c tests/harness/mips/boot.c -o "$L/boot.o" &&
"$EMBCC" --target=mipsel-none-elf -c tests/harness/mips/io.c -o "$L/io.o" ||
    fail "the harness does not compile"
cat > "$out/one.sh" <<'EOT'
opt=$1; c=$2; out=$3; q=$4
L=$out/lib
n=$(basename "$c" .c); o=$out/$opt/$n
if ! "$EMBCC" --target=mipsel-none-elf $opt -Ilib/libc/include -c "$c" -o $o.o 2>/dev/null ||
   ! EMBCC_MIPS_HARNESS=$L sh tests/harness/mips/link.sh $o.elf $o.o $L/libc.a $L/librt.a > /dev/null 2>&1; then
    echo "NA $opt $n"; exit 0
fi
"$EMBSIM" $o.elf --board malta --max-insns 4000000000 > $o.sim 2> $o.err; es=$?
if [ "$q" = - ]; then
    echo "SIM $opt $n $es"; exit 0
fi
sh tests/harness/mips/run.sh $o.elf > $o.qemu 2>/dev/null
if ! grep -q '==EXIT' $o.qemu; then
    echo "NA $opt $n (QEMU did not finish)"; exit 0
fi
if [ $es != 0 ] || ! cmp -s $o.qemu $o.sim; then
    echo "DIFF $opt $n: EmbSim exits $es$(cmp -s $o.qemu $o.sim || echo ', and the output differs') $(head -c 200 $o.err | tr '\n' ' ')"
    exit 0
fi
echo "SAME $opt $n"
EOT
q=$QEMU; [ $have_qemu = 1 ] || q=-
for opt in -O0 -O2 -Os; do
    mkdir -p "$out/$opt"
    for c in tests/exec/*.c; do echo "$opt $c $out $q"; done
done | xargs -P "${EMBCC_JOBS:-8}" -n 4 sh "$out/one.sh" > "$out/results.txt" 2>&1
total=0
for opt in -O0 -O2 -Os; do
    r=$out/results.txt
    grep "^DIFF $opt " "$r" && fail "$opt: EmbSim and QEMU disagree"
    same=$(grep -c "^SAME $opt " "$r")
    sim=$(grep -c "^SIM $opt " "$r")
    na=$(grep -c "^NA $opt " "$r")
    if [ $sim -gt 0 ]; then
        bad=$(grep "^SIM $opt " "$r" | awk '$4 != 0' | head -3)
        [ -z "$bad" ] || { echo "$bad"; fail "$opt: programs did not end at SOFTRES"; }
        echo "embsim-mips: $opt: $sim programs ran (no QEMU to compare with); $na not applicable"
    else
        echo "embsim-mips: $opt: $same programs as QEMU, output and exit status; $na not applicable"
    fi
    total=$((total + same))
done
[ $have_qemu = 0 ] || [ $total -ge 700 ] || fail "only $total programs were compared"
