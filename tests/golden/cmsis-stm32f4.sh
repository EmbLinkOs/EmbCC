#!/bin/sh
# A CubeMX-shaped STM32F405 project, built by EmbCC alone from the vendors'
# own files and RUN on QEMU's netduinoplus2 (an STM32F405):
#
#   ST's startup_stm32f405xx.s       assembled as it ships (gas-gnu.sh)
#   ST's system_stm32f4xx.c          compiled against ST's stm32f4xx.h and
#   ARM's CMSIS-Core headers          ARM's core_cm4.h/cmsis_gcc.h
#   a CubeMX-style linker script      linked by the driver (-T)
#
# and a main.c written against them: USART1 through ST's register
# definitions, CMSIS intrinsics (__REV, __CLZ, __get_PRIMASK), the CPUID,
# and SysTick_Config with a SysTick_Handler counting real interrupts.
# CMSIS chooses its compiler support by __GNUC__, so the C is compiled
# with -fgnuc-version=4.2.1, as clang presents itself by default.
#
# The vendor files are not in this repository; they are looked for in
# $EMBREF (default ~/EmbRef: git clone ARM-software/CMSIS_5 and
# STMicroelectronics/cmsis-device-f4 there). Without them, SKIP.
set -u
echo "TEST-MARKER cmsis-stm32f4"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
ST=$ref/cmsis-device-f4
CM=$ref/CMSIS_5/CMSIS/Core/Include
[ -f "$ST/Source/Templates/gcc/startup_stm32f405xx.s" ] && [ -f "$CM/core_cm4.h" ] ||
    { echo "SKIP: no CMSIS_5 and cmsis-device-f4 under $ref"; exit 0; }
d=tests/golden/cmsis-stm32f4
out=tests/golden/out/cmsis-stm32f4
rm -rf "$out"; mkdir -p "$out"
T=--target=thumbv7em-none-eabi
CF="-O2 -I$ST/Include -I$CM -DSTM32F405xx -fgnuc-version=4.2.1"
fail() { echo "FAIL: $*"; exit 1; }

"$EMBCC" $T -c "$ST/Source/Templates/gcc/startup_stm32f405xx.s" -o "$out/startup.o" ||
    fail "ST's startup file does not assemble"
"$EMBCC" $T $CF -c "$ST/Source/Templates/system_stm32f4xx.c" -o "$out/system.o" ||
    fail "ST's system_stm32f4xx.c does not compile with the CMSIS headers"
"$EMBCC" $T $CF -c "$d/main.c" -o "$out/main.o" || fail "main.c"
"$EMBCC" $T -T "$d/f405.ld" "$out/startup.o" "$out/system.o" "$out/main.o" \
    -o "$out/fw.elf" || fail "the firmware does not link"
echo "ST's startup and system files and CMSIS-Core compile, assemble and link"

command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU not found; not run"; exit 0; }
sh tests/harness/qrun.sh 10 --until done "$QEMU" -M netduinoplus2 -nographic \
    -kernel "$out/fw.elf" > "$out/run.txt" 2>&1
printf 'STM32F405 via CMSIS\n00f42400 00001234 00000000 44332211 0000000f 00000000 00000c24 \nticks 00000001 \ndone\n' > "$out/want.txt"
tr -d '\r' < "$out/run.txt" | head -4 > "$out/got.txt"
cmp -s "$out/got.txt" "$out/want.txt" ||
    { echo "got:"; cat "$out/run.txt"; fail "the firmware did not run as written"; }
echo "and it runs on the STM32F405: 16 MHz, .data and .bss, CMSIS intrinsics,"
echo "a Cortex-M4 CPUID, and SysTick interrupts"
