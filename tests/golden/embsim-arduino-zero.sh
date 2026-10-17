#!/bin/sh
# embsim --board arduino-zero: the Arduino Zero's ATSAMD21G18A, against its
# datasheet (Atmel-42181G) where QEMU has no SAM D21, and against QEMU's
# micro:bit (a Cortex-M0, the same ARMv6-M) for the core.
#
#  1. EmbCC builds for it from Microchip's ATSAMD21G18A.svd: embsvd's
#     header, startup and linker script (256 KiB of flash at 0, 32 KiB of
#     RAM).
#  2. embsim-arduino-zero/zero.c on EmbSim: the reset values of PM,
#     SYSCTRL, NVMCTRL, PORT and GCLK's generators and generic clocks (read
#     through their IDs, tables 14-5, 14-9, 14-13); a SERCOM whose APB
#     clock is masked; GCLK written back (DIVSEL, generator 2's five DIV
#     bits); PM's CKRDY; TC3's COUNT before and after a read request; the
#     CPU's clock measured against OSCULP32K -- 1 MHz out of reset, 8 MHz
#     from OSC8M, halved by CPUDIV, on XOSC32K only once it runs, 48 MHz
#     from the DFLL48M in open loop (with the flash's wait-state warning),
#     47.972 MHz in closed loop on XOSC32K x 1464, its lock 200 us after
#     the reference runs and not before (warned, DFLLRCS); a generator
#     divided by DIV and by 2^(DIV+1); XOSC32K's start-up of 2048 + 3
#     cycles and SYSCTRL's INTFLAG; PORT's input buffer, pulls, toggles,
#     WRCONFIG, the IOBUS, a byte write, a pin let go by PMUXEN; SERCOM5's
#     flags, DRE without TXEN, nothing sent without its core clock, its
#     enable-protected BAUD and MODE; TC4's MFRQ wrap and match flags (a
#     match as the counter leaves CCx), its enable-protected PRESCALER,
#     STOP, RETRIGGER, one-shot, counting down, counting up from above the
#     top to the maximum, COUNT8's PER, COUNT32 with TC5 its slave; and
#     TC3's interrupt toggling L every
#     1 ms of the DFLL's clock. Its values must be zero.txt, the
#     datasheet's; stderr the LEDs and the three warnings (zero-err.txt).
#  3. The board's input: SERCOM5's receiver reads --input, two bytes in
#     its buffer and the third waiting, emptied by RXEN off. Built with
#     ERRATUM_9905, a DFLLMUL write before the DFLL is requested freezes
#     the part: the run ends there (status 3).
#  4. With QEMU: the core. The exec corpus runs on the Zero (its SRAM made
#     64 KiB, --ram-size) and on QEMU's micro:bit (as much), the same image
#     on both: the same output and exit status, the instruction and cycle
#     counts apart by the boots' constant alone.
# ATSAMD21G18A.svd is looked for in $EMBREF/svd; without it the test is
# skipped (SKIP is printed).
set -u
echo "TEST-MARKER embsim-arduino-zero"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
EMBLD=${EMBLD:-./embld}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
d=tests/golden/embsim-arduino-zero
out=tests/golden/out/embsim-arduino-zero
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"
SVD=$ref/svd/ATSAMD21G18A.svd
[ -f "$SVD" ] || { echo "SKIP: no ATSAMD21G18A.svd in $ref/svd"; exit 0; }
EMBSIM_SVD_PATH=/nonexistent:$(dirname "$SVD"); export EMBSIM_SVD_PATH

# ---- 1. the build ---------------------------------------------------------------
T=--target=thumbv6m-none-eabi
"$EMBSVD" "$SVD" --no-cmsis --header "$out/samd21.h" --startup "$out/startup.c" \
    --ld "$out/zero.ld" --flash 0:256K --ram 0x20000000:32K > /dev/null || fail "embsvd"
"$EMBCC" $T -O2 -I"$out" -c "$out/startup.c" -o "$out/startup.o" &&
"$EMBCC" $T -O2 -I"$out" -c $d/zero.c -o "$out/zero.o" &&
"$EMBCC" $T -T "$out/zero.ld" "$out/startup.o" "$out/zero.o" -o "$out/zero.elf" &&
"$EMBCC" $T -O2 -I"$out" -DERRATUM_9905 -c $d/zero.c -o "$out/erratum.o" &&
"$EMBCC" $T -T "$out/zero.ld" "$out/startup.o" "$out/erratum.o" -o "$out/erratum.elf" ||
    fail "zero.c does not build"

# ---- 2. the part ------------------------------------------------------------------
"$EMBSIM" "$out/zero.elf" --board arduino-zero --max-insns 100000000 > "$out/zero.txt" 2> "$out/zero.err"
st=$?
[ $st = 0 ] || { cat "$out/zero.txt" "$out/zero.err"; fail "zero.c exits $st"; }
cmp -s "$out/zero.txt" $d/zero.txt ||
    { diff $d/zero.txt "$out/zero.txt"; fail "the SAM D21 is not as its datasheet has it (zero.txt)"; }
sed 's/ at [0-9.]* ms$//; s/at pc 0x[0-9a-f]*: /at pc N: /' "$out/zero.err" > "$out/zero-err.txt"
cmp -s "$out/zero-err.txt" $d/zero-err.txt ||
    { diff $d/zero-err.txt "$out/zero-err.txt"; fail "stderr: the LEDs and the warnings (zero-err.txt)"; }
# L, toggled by TC3 every 750 counts of 47.972352 MHz / 64: 1.000576 ms
grep '^arduino-zero: L' "$out/zero.err" | tail -6 | sed 's/.* at \([0-9.]*\) ms$/\1/' |
    awk 'NR > 1 { d = $1 - p; if (d < 0.9985 || d > 1.0025) bad = 1 } { p = $1 }
         END { exit bad || NR != 6 }' ||
    { grep '^arduino-zero: L' "$out/zero.err"; fail "TC3's 1 ms are not 1 ms of the DFLL's clock"; }
nm=$(grep -c '^M ' $d/zero.txt)
echo "embsim-arduino-zero: zero.c's $nm values as the SAM D21's datasheet has them, L toggled every 1 ms"

# ---- 3. the input, and erratum 9905 -------------------------------------------------
printf 'abc' > "$out/in.txt"
"$EMBSIM" "$out/zero.elf" --board arduino-zero --max-insns 100000000 --input "$out/in.txt" \
    > "$out/input.txt" 2> /dev/null
grep '^rx' "$out/input.txt" > "$out/rx.txt"
printf 'rx bytes 00006162\nrx more 00000001\nrx flushed 00000000\n' > "$out/rx-want.txt"
cmp -s "$out/rx.txt" "$out/rx-want.txt" ||
    { diff "$out/rx-want.txt" "$out/rx.txt"; fail "SERCOM5's receiver does not read --input"; }
"$EMBSIM" "$out/erratum.elf" --board arduino-zero --max-insns 100000000 > "$out/erratum.txt" 2> "$out/erratum.err"
st=$?
[ $st = 3 ] && grep -q 'erratum 9905' "$out/erratum.err" && grep -qx before "$out/erratum.txt" &&
    ! grep -q '^after' "$out/erratum.txt" ||
    { cat "$out/erratum.txt" "$out/erratum.err"; fail "DFLLMUL written before the DFLL is requested does not freeze the part (status $st)"; }
echo "embsim-arduino-zero: SERCOM5's receiver reads --input; erratum 9905's freeze"

# ---- 4. QEMU: the core ---------------------------------------------------------------
if ! command -v "$QA" > /dev/null 2>&1 || ! "$QA" -M help 2>/dev/null | grep -q '^microbit '; then
    echo "embsim-arduino-zero: no QEMU micro:bit: the core is not compared"
    exit 0
fi
. tests/harness/embsim-corpus.sh
CORP=$out/corpus CTRIPLE=thumbv6m-none-eabi CFLAGS_C= CBOOT=tests/golden/embsim-pico/corpus-boot.c
CEXTRA= QLD=tests/golden/embsim-pico/corpus-microbit.ld ELD=tests/golden/embsim-pico/corpus-microbit.ld CMIN=240
QARGS="-M microbit -global nrf51-soc.sram-size=65536" EARGS="--board arduino-zero --ram-size 64K"
why=$(corpus_run); r=$?
[ $r = 2 ] && { echo "embsim-arduino-zero: $why: the core is not compared with QEMU's"; exit 0; }
[ $r = 0 ] || { echo "$why"; fail "the exec corpus on the Zero and on QEMU's micro:bit"; }
echo "embsim-arduino-zero: the exec corpus (ARMv6-M) on the Zero's memory, against QEMU's micro:bit: $(cat "$CORP/summary")"
