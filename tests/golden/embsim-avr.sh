#!/bin/sh
# embsim-avr: EmbSim's AVR core (tools/embsim/avr.c), refereed by QEMU's
# uno (the ATmega328P).
#
#  1. The exec corpus at -O0, -O2 and -Os: every tests/exec program that
#     builds for the AVR (no libc: one that calls printf does not link,
#     and is not applicable) is linked with tests/harness/avr and lib/rt,
#     and a driver prints main's result. On QEMU (-M uno) and on EmbSim
#     (--board uno) the output must be the same, and so must the
#     instruction count: QEMU's plugin (tools/bench/icount.c) counts the
#     AVR's instructions one at a time, to the driver's last function
#     (stop=), and counts an instruction a skip passes over, which EmbSim
#     counts apart (--count's third line); every program must show the
#     difference a main that only returns shows. The image never exits,
#     so QEMU runs under qrun.sh --until the driver's sentinel, stopped
#     with TERM (EMBCC_QRUN_TERM) so the plugin writes its counts.
#  2. The cycles, which QEMU does not model: tests/golden/embsim/avr-cycles.S
#     runs sequences whose cycles the datasheet gives (every class of
#     instruction, a branch taken and not, skips over one- and two-word
#     instructions, calls and returns, an interrupt's entry and RETI, and
#     SLEEP woken by the timer), and --count must say the sum.
#  3. Instruction edges (tests/golden/embsim/avr-isa.c, built by clang):
#     SREG's H, S, V, N, Z and C after every arithmetic instruction over
#     every operand pair, the carry-in forms with C and Z set and clear,
#     the multiplies' R1:R0, the 16-bit ADIW and SBIW, the shifts,
#     X/Y/Z's pre-decrement and post-increment, LPM's post-increment,
#     the skips, and Timer/Counter1's interrupt; printed as hashes, it
#     must print what QEMU prints and the record (avr-isa.txt).
#  4. The ends of a run (no QEMU needed): the harness's loop after main,
#     SLEEP with nothing to wake it, an instruction the part does not
#     have, --until and --max-insns.
set -u
echo "TEST-MARKER embsim-avr"
. "$(dirname "$0")/../lib.sh"

QAVR=${EMBCC_QEMU_AVR:-qemu-system-avr}
NM=${EMBCC_LLVM_NM:-llvm-nm}
EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
export EMBCC EMBLD EMBSIM NM QAVR
out=tests/golden/out/embsim-avr
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

have_qemu=0
command -v "$QAVR" >/dev/null 2>&1 && have_qemu=1
plug=
if [ $have_qemu = 1 ] && command -v "$NM" >/dev/null 2>&1; then
    inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    p=$PWD/$out/icount.so
    { cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -undefined dynamic_lookup -o "$p" tools/bench/icount.c ||
      cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -o "$p" tools/bench/icount.c; } 2>/dev/null && plug=$p
fi
[ $have_qemu = 0 ] || [ -n "$plug" ] ||
    echo "note: QEMU's counting plugin (or $NM) is missing; counts are not compared"

# the harness, the runtime and the driver
H=$out/h; mkdir -p "$H"
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$H/io.o" ||
    fail "the AVR harness does not build"
sh tools/build-rt.sh avr "$H" > "$H/build.log" 2>&1 || fail "lib/rt does not build for avr"
cat > "$out/drv.c" <<'EOT'
/* main's result printed; then the driver's last function, where QEMU's
 * plugin stops counting, prints the sentinel and waits for ever with
 * interrupts off */
int prog_main(void);
void putn(long v);
void __embsim_end(void);
int main(void)
{
    putn((long)prog_main());
    __embsim_end();
    return 0;
}
EOT
cat > "$out/end.c" <<'EOT'
void puts_(const char *s);
void __embsim_end(void)
{
    puts_("\n==END==\n");
    __asm__ volatile("cli");
    for (;;)
        ;
}
EOT
for f in drv end; do
    "$EMBCC" --target=avr -O1 -c "$out/$f.c" -o "$H/$f.o" || fail "the driver does not build"
done

# ---- 4. the ends of a run ----------------------------------------------------
e=$out/ends; mkdir -p "$e"
cat > "$e/ends.c" <<'EOF'
/* MODE 0: main returns into the harness's loop; 1: SLEEP (SMCR.SE)
 * with interrupts off; 2: an ELPM (bad.S), which the ATmega328P does
 * not have; 3: prints a mark and loops for ever printing */
void puts_(const char *s);
void bad(void);
int main(void)
{
    puts_("ready\n");
#if MODE == 1
    *(volatile unsigned char *)0x53 = 1;
    __asm__ volatile("cli\n\tsleep");
    puts_("woke\n");
#elif MODE == 2
    bad();
#elif MODE == 3
    for (volatile int i = 0;; i++)
        if (i == 100)
            puts_("==MARK==\n");
#endif
    return 0;
}
EOF
printf '\t.text\n\t.globl bad\nbad:\n\telpm\n\tret\n' > "$e/bad.S"
"$EMBCC" --target=avr -c "$e/bad.S" -o "$e/bad.o" || fail "bad.S does not assemble"
for m in 0 1 2 3; do
    "$EMBCC" --target=avr -O1 -DMODE=$m -c "$e/ends.c" -o "$e/e$m.o" &&
    EMBCC_AVR_HARNESS=$H sh tests/harness/avr/link.sh "$e/e$m.elf" "$e/e$m.o" \
        "$e/bad.o" > /dev/null 2>&1 || fail "the end-of-run images do not build"
done
"$EMBSIM" "$e/e0.elf" --board uno --stats --max-insns 1000000 > "$e/0.out" 2>&1; st=$?
[ $st = 0 ] && grep -q ready "$e/0.out" && grep -q 'nothing can interrupt' "$e/0.out" ||
    { cat "$e/0.out"; fail "the harness's loop after main does not end the run (status $st)"; }
"$EMBSIM" "$e/e1.elf" --board uno --stats --max-insns 1000000 > "$e/1.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'asleep with nothing to wake it' "$e/1.out" &&
    ! grep -q woke "$e/1.out" || { cat "$e/1.out"; fail "SLEEP with interrupts off does not end the run (status $st)"; }
"$EMBSIM" "$e/e2.elf" --board uno --max-insns 1000000 > "$e/2.out" 2>&1; st=$?
[ $st = 3 ] && grep -q '0x95d8 at 0x[0-9a-f]* is not an ATmega328P instruction' "$e/2.out" ||
    { cat "$e/2.out"; fail "an ELPM is not stopped as an instruction the part lacks (status $st)"; }
"$EMBSIM" "$e/e3.elf" --board uno --until '==MARK==' --max-insns 10000000 > "$e/3.out" 2>&1; st=$?
[ $st = 0 ] && [ "$(grep -c MARK "$e/3.out")" = 1 ] || fail "--until does not stop at the first ==MARK== (status $st)"
"$EMBSIM" "$e/e3.elf" --board uno --max-insns 500 > "$e/4.out" 2>&1; st=$?
[ $st = 4 ] && grep -q '500 instructions' "$e/4.out" || { cat "$e/4.out"; fail "--max-insns 500: status $st"; }
echo "embsim-avr: the loop after main, SLEEP that nothing wakes, an instruction the part lacks, --until and --max-insns end a run as documented"

# ---- 2. the cycles, against the datasheet ------------------------------------------
c=$out/cycles; mkdir -p "$c"
printf '\t.text\n\t.globl main\nmain:\n\tret\n' > "$c/empty.S"
for f in tests/golden/embsim/avr-cycles.S "$c/empty.S"; do
    b=$(basename "$f" .S)
    "$EMBCC" --target=avr -c "$f" -o "$c/$b.o" &&
    EMBCC_AVR_HARNESS=$H sh tests/harness/avr/link.sh "$c/$b.elf" "$c/$b.o" > /dev/null 2>&1 ||
        fail "$f does not build"
    "$EMBSIM" "$c/$b.elf" --board uno --count "$c/$b.count" --max-insns 100000 > /dev/null 2>&1 ||
        fail "$f does not run to its end on EmbSim"
done
want=$(sed -n 's/.*;c \([0-9][0-9 ]*\).*/\1/p' tests/golden/embsim/avr-cycles.S |
       awk '{ for (i = 1; i <= NF; i++) s += $i } END { print s }')
got=$(( $(sed -n 2p "$c/avr-cycles.count") - $(sed -n 2p "$c/empty.count") ))
[ "$got" = "$want" ] || fail "avr-cycles.S takes $got cycles on EmbSim; the datasheet says $want"
echo "embsim-avr: avr-cycles.S's $want cycles are the datasheet's: every instruction class, branches taken and not, skips over one and two words, calls and returns, an interrupt's entry and RETI"

# ---- 3. instruction edges ----------------------------------------------------------
x=$out/isa; mkdir -p "$x"
"$EMBCC" --target=avr -c tests/golden/embsim/avr-isa.S -o "$x/k.o" || fail "avr-isa.S does not assemble"
for v in isa isan; do
    D=; [ $v = isan ] && D=-DNO_TIMER
    # shellcheck disable=SC2086
    "$EMBCC" --target=avr -Os $D -c tests/golden/embsim/avr-isa.c -o "$x/$v.o" &&
    EMBCC_AVR_HARNESS=$H sh tests/harness/avr/link.sh "$x/$v.elf" "$x/$v.o" "$x/k.o" \
        "$H/end.o" "$H/librt.a" > /dev/null 2>&1 || fail "avr-isa.c does not build ($v)"
    "$EMBSIM" "$x/$v.elf" --board uno --count "$x/$v.en" --max-insns 100000000 \
        > "$x/$v.sim" 2> "$x/$v.err" || { cat "$x/$v.err"; fail "avr-isa.c ($v): EmbSim fails"; }
done
cmp -s "$x/isa.sim" tests/golden/embsim/avr-isa.txt || {
    diff tests/golden/embsim/avr-isa.txt "$x/isa.sim" | head -20
    fail "avr-isa.c: EmbSim's output is not avr-isa.txt"; }
if [ $have_qemu = 1 ]; then
    for v in isa isan; do
        pl=
        if [ -n "$plug" ] && [ $v = isan ]; then
            stop=$("$NM" "$x/$v.elf" | sed -n 's/^0*\([0-9a-f]*\) T __embsim_end$/0x\1/p')
            pl="-plugin $plug,out=$x/$v.qn,stop=$stop"
        fi
        # shellcheck disable=SC2086
        EMBCC_QRUN_TERM=1 sh tests/harness/qrun.sh 120 --until '==END==' "$QAVR" -M uno \
            -nographic -bios "$x/$v.elf" $pl > "$x/$v.qemu" 2> /dev/null
        cmp -s "$x/$v.qemu" "$x/$v.sim" || {
            diff "$x/$v.qemu" "$x/$v.sim" | head -20
            fail "avr-isa.c ($v): EmbSim's output is not QEMU's"; }
    done
fi
echo "embsim-avr: the instruction edges ($(wc -l < "$x/isa.sim" | tr -d ' ') results) as QEMU and the record have them"

# ---- 1. the exec corpus --------------------------------------------------------
cat > "$out/one.sh" <<'EOT'
opt=$1; c=$2; out=$3
H=$out/h; n=$(basename "$c" .c); o=$out/p$opt/$n
if ! "$EMBCC" --target=avr $opt -Dmain=prog_main -Ilib/libc/include -c "$c" -o $o.o 2>/dev/null ||
   ! EMBCC_AVR_HARNESS=$H sh tests/harness/avr/link.sh $o.elf $o.o $H/drv.o \
       $H/end.o $H/librt.a > /dev/null 2>&1; then
    echo "NA $opt $n"; exit 0
fi
# QEMU first: a program it cannot finish (a 2 KiB part runs out of stack
# for some; div-const's loops take billions of instructions) is not
# applicable, without EmbSim's spending its budget on it too
if [ "$HAVE_QEMU" = 1 ]; then
    pl=
    if [ -n "$PLUG" ]; then
        stop=$("$NM" $o.elf | sed -n 's/^0*\([0-9a-f]*\) T __embsim_end$/0x\1/p')
        pl="-plugin $PLUG,out=$o.qn,stop=$stop"
    fi
    # shellcheck disable=SC2086
    EMBCC_QRUN_TERM=1 sh tests/harness/qrun.sh 60 --until '==END==' "$QAVR" -M uno \
        -nographic -bios $o.elf $pl > $o.qout 2> /dev/null
    if ! grep -q '==END==' $o.qout; then
        echo "NA $opt $n (QEMU did not finish)"; exit 0
    fi
fi
"$EMBSIM" $o.elf --board uno --max-insns 4000000000 --count $o.en > $o.eout 2> $o.eerr; es=$?
if [ $es != 0 ] || ! grep -q '==END==' $o.eout; then
    echo "DIFF $opt $n: EmbSim exits $es without the sentinel $(head -c 200 $o.eerr | tr '\n' ' ')"
    exit 0
fi
if [ "$HAVE_QEMU" = 0 ]; then
    echo "SIM $opt $n"; exit 0
fi
if ! cmp -s $o.qout $o.eout; then
    echo "DIFF $opt $n: the output differs"; exit 0
fi
if [ -s "$o.qn" ]; then
    qi=$(sed -n 1p $o.qn); ei=$(sed -n 1p $o.en); sk=$(sed -n 3p $o.en)
    echo "COUNT $opt $n $((qi - ei - sk)) $ei $sk"
else
    echo "SAME $opt $n"
fi
EOT
export HAVE_QEMU=$have_qemu PLUG=$plug
for opt in -O0 -O2 -Os; do
    mkdir -p "$out/p$opt"
    printf 'int main(void) { return 0; }\n' > "$out/p$opt/empty.c"
    sh "$out/one.sh" $opt "$out/p$opt/empty.c" "$out" > "$out/p$opt/empty.result"
done
for opt in -O0 -O2 -Os; do
    for c in tests/exec/*.c; do echo "$opt $c $out"; done
done | xargs -P "${EMBCC_JOBS:-8}" -n 3 sh "$out/one.sh" > "$out/results.txt" 2>&1

total=0
for opt in -O0 -O2 -Os; do
    r=$out/results.txt
    base=$(cat "$out/p$opt/empty.result")
    case $base in
        COUNT*) set -- $base; bi=$4 ;;
        SAME*|SIM*) bi= ;;
        *) fail "$opt: a main that returns does not run: $base" ;;
    esac
    grep "^DIFF $opt " "$r" && fail "$opt: EmbSim and QEMU disagree"
    if [ -n "$bi" ]; then
        bad=$(grep "^COUNT $opt " "$r" | awk -v i="$bi" '$4 != i')
        [ -z "$bad" ] || { echo "$bad" | head -5
            fail "$opt: QEMU's count differs from EmbSim's (with the skipped) by other than the end's $bi"; }
    fi
    same=$(grep -c "^\(COUNT\|SAME\) $opt " "$r")
    sim=$(grep -c "^SIM $opt " "$r")
    na=$(grep -c "^NA $opt " "$r")
    insns=$(grep "^COUNT $opt " "$r" | awk '{ s += $5 } END { print s + 0 }')
    skips=$(grep "^COUNT $opt " "$r" | awk '{ s += $6 } END { print s + 0 }')
    if [ $sim -gt 0 ]; then
        echo "embsim-avr: $opt: $sim programs ran (no QEMU to compare with); $na not applicable"
    elif [ -n "$bi" ]; then
        echo "embsim-avr: $opt: $same programs as QEMU, to the instruction ($insns instructions, $skips skipped); $na not applicable"
    else
        echo "embsim-avr: $opt: $same programs as QEMU, output; $na not applicable"
    fi
    total=$((total + same))
done
[ $have_qemu = 0 ] || [ $total -ge 300 ] || fail "only $total programs were compared"

# avr-isa.c's count (without the timer, whose wait is QEMU's time): its
# end is the driver's, so the difference is the corpus's
if [ $have_qemu = 1 ] && [ -s "$x/isan.qn" ]; then
    set -- $(cat "$out/p-Os/empty.result")
    qi=$(sed -n 1p "$x/isan.qn"); ei=$(sed -n 1p "$x/isan.en"); sk=$(sed -n 3p "$x/isan.en")
    [ $((qi - ei - sk)) = "$4" ] ||
        fail "avr-isa.c: QEMU counts $qi instructions, EmbSim $ei and $sk skipped: not the end's $4"
    echo "embsim-avr: avr-isa.c to the instruction: $ei run and $sk skipped, QEMU's $qi"
fi
