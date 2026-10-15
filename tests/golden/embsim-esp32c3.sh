#!/bin/sh
# embsim --board esp32c3-devkitm-1: Espressif's ESP32-C3-DevKitM-1, against
# the ESP32-C3 Technical Reference Manual (v1.4) where QEMU has no ESP32-C3,
# and against QEMU's virt with the C3's instruction set (RV32IMC) for the
# core.
#
#  1. EmbCC builds for it from Espressif's esp32c3.svd: embsvd's header,
#     startup (the vectored trap table, 256-aligned) and linker script (the
#     code in IRAM, the rest in DRAM, as the ROM's loader places them);
#     embsvd refuses --flash for it, and a RISC-V part's startup it cannot
#     write.
#  2. embsim-esp32c3/c3.c on EmbSim: the core's CSRs (misa, the IDs,
#     mtvec's vectored mode, mstatus.MPP, the performance counter, the
#     dedicated GPIO's), its exceptions (a CSR it lacks, a debug CSR, a
#     read-only one written, an AMO, ecall, ebreak) with mcause and mtval;
#     UART0's reset values and interrupt bits; SYSTEM's clocks (20, 40, 80
#     and 160 MHz, measured against SYSTIMER's 16 MHz); SYSTIMER's units,
#     loads and comparators (target mode once, a passed target at once, the
#     2^51 - 1 boundary, an alarm 0.1 ms out); the interrupt matrix
#     (priorities, the threshold, a priority of 0, disabling, the order the
#     core takes them in, mcause, an edge type's capture and its clearing
#     claimed and unclaimed, a level type taken again, WFI woken with MIE
#     off, the IDLE count); the IO_MUX pads' reset values pin by pin, GPIO
#     out, in, inverted, open drain with and without a pull, a pad not in
#     its GPIO function, a pad without input. Its values must be c3.txt,
#     the TRM's; stderr the board's RGB LED, which SYSTIMER's period
#     interrupt sends a colour every 1 ms, decoded from GPIO8's pulses, and
#     a pulse that is no SK6812 bit; the run ends with RTC_CNTL's software
#     reset (status 0).
#  3. The board's inputs: UART0's receiver reads --input; BOOT pressed
#     (--set) pulls GPIO9 low.
#  4. With QEMU: the core. The exec corpus runs on EmbSim's board and on
#     QEMU's virt with an rv32i,m,c,zicsr,zifencei core, the same code
#     linked for each (corpus-esp32c3.ld, corpus-virt.ld; RISC-V
#     semihosting for the output and the exit): the same output and exit
#     status, the instruction and cycle counts apart by the boots'
#     constant alone. A program that traps (EmbCC's libc locks with LR/SC,
#     which neither core has) ends with 125 on both, and is not counted.
# esp32c3.svd is looked for in $EMBREF/svd; without it the test is
# skipped (SKIP is printed).
set -u
echo "TEST-MARKER embsim-esp32c3"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
EMBLD=${EMBLD:-./embld}
QR=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
ref=${EMBREF:-$HOME/EmbRef}
d=tests/golden/embsim-esp32c3
out=tests/golden/out/embsim-esp32c3
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"
SVD=$ref/svd/esp32c3.svd
[ -f "$SVD" ] || { echo "SKIP: no esp32c3.svd in $ref/svd"; exit 0; }
EMBSIM_SVD_PATH=/nonexistent:$(dirname "$SVD"); export EMBSIM_SVD_PATH

# ---- 1. the build ---------------------------------------------------------------
T=--target=riscv32-unknown-elf
"$EMBSVD" "$SVD" --no-cmsis --header "$out/esp32c3.h" --startup "$out/startup.c" \
    --ld "$out/c3.ld" > /dev/null || fail "embsvd"
grep -q 'NonMaskableInt_IRQn\|SysTick_IRQn' "$out/esp32c3.h" &&
    fail "the ESP32-C3's header has the Cortex-M's exceptions"
grep -q 'UART0_IRQn = 21,' "$out/esp32c3.h" ||
    fail "the header's IRQn are not the interrupt matrix's sources (UART0 21)"
"$EMBCC" $T -O2 -I"$out" -c "$out/startup.c" -o "$out/startup.o" &&
"$EMBCC" $T -O2 -I"$out" -c $d/c3.c -o "$out/c3.o" &&
"$EMBCC" $T -T "$out/c3.ld" "$out/startup.o" "$out/c3.o" -o "$out/c3.elf" ||
    fail "c3.c does not build"
vt=$(llvm-nm "$out/c3.elf" | awk '$3 == "_vector_table" { print $1 }')
st=$(llvm-nm "$out/c3.elf" | awk '$3 == "_start" { print $1 }')
[ -n "$vt" ] && [ $((0x$vt % 256)) = 0 ] || fail "the vector table is at 0x$vt, not 256-aligned"
llvm-readelf -h "$out/c3.elf" | grep -q "Entry point address: *0x$st" ||
    fail "the entry is not _start (0x$st)"
llvm-readelf -l "$out/c3.elf" | awk '$1 == "LOAD" { print $3 }' > "$out/segs.txt"
grep -q '^0x4038' "$out/segs.txt" && grep -q '^0x3fc' "$out/segs.txt" ||
    { cat "$out/segs.txt"; fail "the image is not in IRAM and DRAM"; }
"$EMBSVD" "$SVD" --ld "$out/x.ld" --flash 0x42000000:4M --ram 0x3fc80000:320K \
    > "$out/flash.txt" 2>&1 && fail "embsvd took --flash for the ESP32-C3"
grep -q 'takes no --flash or --ram' "$out/flash.txt" || { cat "$out/flash.txt"; fail "the --flash refusal"; }
sed 's|<name>ESP32-C3</name>|<name>OTHER-RV</name>|' "$SVD" > "$out/other.svd"
"$EMBSVD" "$out/other.svd" --startup "$out/x.c" > "$out/other.txt" 2>&1 &&
    fail "embsvd wrote a startup for a RISC-V part it does not know"
grep -q "the ESP32-C3's startup only" "$out/other.txt" || { cat "$out/other.txt"; fail "the RISC-V startup refusal"; }
echo "embsim-esp32c3: embsvd's header, startup and IRAM/DRAM script build c3.c; the vector table is 256-aligned"

# ---- 2. the part ------------------------------------------------------------------
"$EMBSIM" "$out/c3.elf" --board esp32c3-devkitm-1 --max-insns 50000000 > "$out/c3.txt" 2> "$out/c3.err"
st=$?
[ $st = 0 ] || { cat "$out/c3.txt" "$out/c3.err"; fail "c3.c exits $st"; }
cmp -s "$out/c3.txt" $d/c3.txt ||
    { diff $d/c3.txt "$out/c3.txt"; fail "the ESP32-C3 is not as its TRM has it (c3.txt)"; }
sed 's/ at [0-9.]* ms$//; s/a high of [0-9.]* us/a high of N us/' "$out/c3.err" > "$out/c3-err.txt"
cmp -s "$out/c3-err.txt" $d/c3-err.txt ||
    { diff $d/c3-err.txt "$out/c3-err.txt"; fail "stderr: the RGB LED (c3-err.txt)"; }
# the colour and off in turn, every 1 ms of SYSTIMER's 16 MHz
grep 'RGB LED [#o]' "$out/c3.err" | sed 's/.* at \([0-9.]*\) ms$/\1/' |
    awk 'NR > 1 { d = $1 - p; if (d < 0.9995 || d > 1.0005) bad = 1 } { p = $1 }
         END { exit bad || NR != 6 }' ||
    { cat "$out/c3.err"; fail "the LED's frames are not 1 ms of SYSTIMER apart"; }
nm=$(grep -c '^M ' $d/c3.txt)
echo "embsim-esp32c3: c3.c's $nm values as the ESP32-C3's TRM has them; the RGB LED's frames every 1 ms"

# ---- 3. the board's inputs -----------------------------------------------------------
printf 'abc' > "$out/in.txt"
"$EMBSIM" "$out/c3.elf" --board esp32c3-devkitm-1 --max-insns 50000000 --input "$out/in.txt" \
    > "$out/input.txt" 2> /dev/null
grep '^rx' "$out/input.txt" > "$out/rx.txt"
printf 'rx count 00000003\nrx full-over-2 00000001\nrx full-over-3 00000000\nrx bytes 00006162\nrx left 00000001\nrx reset 00000000\n' > "$out/rx-want.txt"
cmp -s "$out/rx.txt" "$out/rx-want.txt" ||
    { diff "$out/rx-want.txt" "$out/rx.txt"; fail "UART0's receiver does not read --input"; }
"$EMBSIM" "$out/c3.elf" --board esp32c3-devkitm-1 --max-insns 50000000 --set BOOT=pressed \
    > "$out/button.txt" 2> /dev/null
grep -qx 'M gpio-in 0013f040' "$out/button.txt" ||
    { grep gpio-in "$out/button.txt"; fail "BOOT pressed does not pull GPIO9 low"; }
echo "embsim-esp32c3: UART0's receiver reads --input (RXFIFO_FULL above its threshold, RXFIFO_RST); BOOT pressed"

# ---- 4. QEMU: the core -------------------------------------------------------------
if ! command -v "$QR" > /dev/null 2>&1; then
    echo "embsim-esp32c3: no $QR: the core is not compared"
    exit 0
fi
. tests/harness/embsim-corpus.sh
CORP=$out/corpus CTRIPLE=riscv32-unknown-elf CFLAGS_C= CBOOT=$d/corpus-boot.c
CEXTRA= QLD=$d/corpus-virt.ld ELD=$d/corpus-esp32c3.ld CMIN=230 QA=$QR
QARGS="-M virt -cpu rv32i,m=true,c=true,zicsr=true,zifencei=true -bios none"
EARGS="--board esp32c3-devkitm-1 --ram-size 7M"
why=$(corpus_run); r=$?
[ $r = 2 ] && { echo "embsim-esp32c3: $why: the core is not compared with QEMU's"; exit 0; }
[ $r = 0 ] || { echo "$why"; fail "the exec corpus on the ESP32-C3 and on QEMU's RV32IMC virt"; }
echo "embsim-esp32c3: the exec corpus (RV32IMC) on the DevKitM-1's memory, against QEMU's virt: $(cat "$CORP/summary")"
