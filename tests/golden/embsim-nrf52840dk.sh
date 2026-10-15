#!/bin/sh
# embsim --board nrf52840dk: Nordic's nRF52840 DK, against QEMU's nRF51
# (its micro:bit) where the peripheral is the same, and against the
# nRF52840 Product Specification where it is not.
#
#  1. EmbCC builds for it from Nordic's nrf52840.svd: embsvd's header,
#     startup and linker script (ARMv6-M code and 16 KiB of RAM, so one
#     image runs on the micro:bit too).
#  2. embsim-nrf52840dk/dk.c on EmbSim: UART0, the GPIO (PIN_CNF, DIR,
#     OUT, IN with pulls, a disconnected input, open-drain, P1, a button),
#     TIMER0 as a counter (compares, shorts, the 8-bit wrap, stopped),
#     INTENSET and INTENCLR; the nRF52840's own: FICR, CLOCK (the HFXO's
#     256 us debounce timed by TIMER2, the LFCLK), RTC0 (a compare after 32
#     counts of 32768 Hz, PRESCALER read-only while it runs, TICK only
#     with EVTEN, TRIGOVRFLW), TIMER1's 1 ms interrupt toggling LED1,
#     UARTE0's EasyDMA from RAM and its refusal from flash. Its values must
#     be dk.txt, the PS's; its "Q" values what QEMU's micro:bit prints;
#     stderr the LEDs' changes (LED1's 1 ms apart) and EasyDMA's warning.
#  3. UARTE0's receiver reads --input into RAM.
#  4. With QEMU: the core. The exec corpus for the M4 with FPv4-SP on the
#     DK's memory runs on EmbSim's nrf52840dk and QEMU's mps2-an386, the
#     same image: the same output and exit status, the instruction and
#     cycle counts apart by the boots' constant alone.
# nrf52840.svd is looked for in $EMBREF/svd; without it the test is
# skipped (SKIP is printed).
set -u
echo "TEST-MARKER embsim-nrf52840dk"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
EMBLD=${EMBLD:-./embld}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
d=tests/golden/embsim-nrf52840dk
out=tests/golden/out/embsim-nrf52840dk
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"
SVD=$ref/svd/nrf52840.svd
[ -f "$SVD" ] || { echo "SKIP: no nrf52840.svd in $ref/svd"; exit 0; }
EMBSIM_SVD_PATH=/nonexistent:$(dirname "$SVD"); export EMBSIM_SVD_PATH

# ---- 1. the build ---------------------------------------------------------------
T=--target=thumbv6m-none-eabi
"$EMBSVD" "$SVD" --no-cmsis --header "$out/nrf52840.h" --startup "$out/startup.c" \
    --ld "$out/dk.ld" --flash 0:1M --ram 0x20000000:16K > /dev/null || fail "embsvd"
"$EMBCC" $T -O2 -I"$out" -c "$out/startup.c" -o "$out/startup.o" &&
"$EMBCC" $T -O2 -I"$out" -c $d/dk.c -o "$out/dk.o" &&
"$EMBCC" $T -T "$out/dk.ld" "$out/startup.o" "$out/dk.o" -o "$out/dk.elf" ||
    fail "dk.c does not build"

# ---- 2. the part ------------------------------------------------------------------
"$EMBSIM" "$out/dk.elf" --board nrf52840dk --max-insns 50000000 > "$out/dk.txt" 2> "$out/dk.err"
st=$?
[ $st = 0 ] || { cat "$out/dk.txt" "$out/dk.err"; fail "dk.c exits $st"; }
cmp -s "$out/dk.txt" $d/dk.txt ||
    { diff $d/dk.txt "$out/dk.txt"; fail "the DK is not as the nRF52840's PS has it (dk.txt)"; }
sed 's/ at [0-9.]* ms$//; s/PTR 0x[0-9a-f]* is/PTR is/' "$out/dk.err" | grep -v '^embsim: a loop' \
    > "$out/dk-err.txt"
cmp -s "$out/dk-err.txt" $d/dk-err.txt ||
    { diff $d/dk-err.txt "$out/dk-err.txt"; fail "stderr: the LEDs, EasyDMA's warning (dk-err.txt)"; }
# LED1, toggled by TIMER1's compare every 1 ms of its 1 MHz
grep '^nrf52840dk: LED1' "$out/dk.err" | sed -n '3,6s/.* at \([0-9.]*\) ms$/\1/p' |
    awk 'NR > 1 { d = $1 - p; if (d < 0.9995 || d > 1.0005) bad = 1 } { p = $1 }
         END { exit bad || NR != 4 }' ||
    { grep '^nrf52840dk: LED1' "$out/dk.err"; fail "TIMER1's 1 ms are not 1 ms of the board's 64 MHz"; }
nq=$(grep -c '^Q ' $d/dk.txt)
nm=$(grep -c '^M ' $d/dk.txt)
echo "embsim-nrf52840dk: dk.c's $nm values as the nRF52840's PS has them, LED1 toggled every 1 ms"

# ---- 3. the console's input, by EasyDMA ----------------------------------------
printf 'abcdefgh' > "$out/in.txt"
"$EMBSIM" "$out/dk.elf" --board nrf52840dk --max-insns 50000000 --input "$out/in.txt" \
    > "$out/input.txt" 2> /dev/null
grep -qx "rx 'abcd'" "$out/input.txt" ||
    { grep '^rx' "$out/input.txt"; fail "UARTE0's receiver does not put --input in RAM"; }
echo "embsim-nrf52840dk: four bytes of input into RAM by UARTE0's EasyDMA"

# ---- 4. QEMU: the nRF51's peripherals, and the core ---------------------------
if ! command -v "$QA" > /dev/null 2>&1 || ! "$QA" -M help 2>/dev/null | grep -q '^microbit '; then
    echo "embsim-nrf52840dk: no QEMU micro:bit: the $nq Q values are not compared"
    exit 0
fi
sh tests/harness/qrun.sh 20 --until q-end "$QA" -M microbit -nographic \
    -kernel "$out/dk.elf" > "$out/qemu.raw" 2> "$out/qemu.err"
tr -d '\r' < "$out/qemu.raw" > "$out/qemu.txt"
grep -aq '^q-end$' "$out/qemu.txt" || { cat "$out/qemu.txt" "$out/qemu.err"; fail "dk.c's Q part did not finish on QEMU"; }
grep -a '^Q ' "$out/qemu.txt" > "$out/qemu-q.txt"
grep '^Q ' "$out/dk.txt" > "$out/sim-q.txt"
[ "$(grep -c . "$out/qemu-q.txt")" = "$nq" ] || fail "QEMU printed $(grep -c . "$out/qemu-q.txt") Q values, not $nq"
cmp -s "$out/qemu-q.txt" "$out/sim-q.txt" ||
    { diff "$out/qemu-q.txt" "$out/sim-q.txt"; fail "EmbSim's nRF52840 is not QEMU's nRF51 where they are the same"; }
echo "embsim-nrf52840dk: and $nq values as QEMU's micro:bit has them (UART0, GPIO, TIMER0)"

if ! "$QA" -M help 2>/dev/null | grep -q '^mps2-an386 '; then
    echo "embsim-nrf52840dk: no QEMU mps2-an386: the core is not compared"
    exit 0
fi
. tests/harness/embsim-corpus.sh
CORP=$out/corpus CTRIPLE=thumbv7em-none-eabihf CFLAGS_C= CBOOT=tests/golden/embsim-pico/corpus-boot.c
CEXTRA= QLD=$d/corpus-nrf52840.ld ELD=$d/corpus-nrf52840.ld CMIN=240
QARGS="-M mps2-an386 -cpu cortex-m4" EARGS="--board nrf52840dk"
why=$(corpus_run); r=$?
[ $r = 2 ] && { echo "embsim-nrf52840dk: $why: the core is not compared with QEMU's"; exit 0; }
[ $r = 0 ] || { echo "$why"; fail "the exec corpus on the DK and on QEMU's mps2-an386"; }
echo "embsim-nrf52840dk: the exec corpus (M4, FPv4-SP) on the DK's memory, against QEMU's mps2-an386: $(cat "$CORP/summary")"
