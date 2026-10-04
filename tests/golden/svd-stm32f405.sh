#!/bin/sh
# embsvd: a device's header, startup and linker script from its CMSIS-SVD
# file -- checked against the vendor's own header, and run.
#
#   1. From ST's STM32F405.svd, embsvd writes STM32F405.h, a startup and a
#      linker script.
#   2. The same table of 149 register addresses is compiled against ST's
#      stm32f4xx.h and against the generated header, and the two objects'
#      .rodata must be the same bytes: every peripheral's base, and every
#      register's offset in its struct, as ST laid them out by hand.
#   3. A program written against the generated header alone, with the
#      generated startup and linker script, runs on QEMU's netduinoplus2
#      (an STM32F405): USART1 by its registers and field masks, .data and
#      .bss and a constructor by the startup, SysTick and USART1
#      interrupts by the vector table's slots, CMSIS-Core through the
#      header's configuration.
#   4. --list and --show answer from the same file, and a broken SVD is
#      refused with its line.
#
# The SVD and CMSIS are not in this repository; they are looked for in
# $EMBREF (default ~/EmbRef: svd/STM32F405.svd from cmsis-svd-data,
# CMSIS_5, cmsis-device-f4). Without them, SKIP.
set -u
echo "TEST-MARKER svd-stm32f405"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSVD=${EMBSVD:-./embsvd}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
SVD=$ref/svd/STM32F405.svd
CM=$ref/CMSIS_5/CMSIS/Core/Include
ST=$ref/cmsis-device-f4/Include
[ -f "$SVD" ] && [ -f "$CM/core_cm4.h" ] && [ -f "$ST/stm32f4xx.h" ] ||
    { echo "SKIP: no STM32F405.svd, CMSIS_5 and cmsis-device-f4 under $ref"; exit 0; }
command -v "$OBJCOPY" >/dev/null 2>&1 || { echo "SKIP: no $OBJCOPY"; exit 0; }
d=tests/golden/svd-stm32f405
out=tests/golden/out/svd-stm32f405
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
T=--target=thumbv7em-none-eabi
CF="-O2 -I$out -I$CM -fgnuc-version=4.2.1"

# 1. generate
# ST's SVD 1.2 says 3 priority bits and no FPU; the part has 4 and one,
# as ST's own header says. What the file says is what comes out...
"$EMBSVD" "$SVD" --header "$out/asis.h" || fail "embsvd did not read ST's SVD"
grep -q '#define __NVIC_PRIO_BITS 3' "$out/asis.h" &&
grep -q '#define __FPU_PRESENT 0' "$out/asis.h" ||
    fail "the header does not say what the SVD says about the core"
# ...unless corrected
"$EMBSVD" "$SVD" --header "$out/STM32F405.h" --startup "$out/startup.c" \
    --ld "$out/f405.ld" --flash 0x08000000:1M --ram 0x20000000:128K \
    --nvic-prio-bits 4 --fpu-present 1 || fail "embsvd with corrections"
for s in 'typedef enum {' 'USART1_IRQn = 37,' '#define __NVIC_PRIO_BITS 4' \
         '#define __FPU_PRESENT 1' \
         '#include "core_cm4.h"' '} RCC_Type;' \
         '#define USART2 ((USART6_Type \*)USART2_BASE)' \
         '#define RCC_APB2ENR_USART1EN_Pos 4U'; do
    grep -q "$s" "$out/STM32F405.h" || fail "the header has no '$s'"
done
echo "STM32F405.h, startup.c and f405.ld from ST's SVD"

# 2. the generated layout is ST's
"$EMBCC" $T $CF -I"$ST" -DSTM32F405xx '-DHEADER="stm32f4xx.h"' \
    -c "$d/regs.c" -o "$out/st.o" || fail "regs.c against ST's header"
"$EMBCC" $T $CF '-DHEADER="STM32F405.h"' \
    -c "$d/regs.c" -o "$out/gen.o" || fail "regs.c against the generated header"
"$OBJCOPY" -O binary -j .rodata "$out/st.o" "$out/st.bin"
"$OBJCOPY" -O binary -j .rodata "$out/gen.o" "$out/gen.bin"
n=$(wc -c < "$out/st.bin" | tr -d ' ')
[ "$n" -ge 500 ] || fail "the table is $n bytes; it should be 149 addresses"
cmp -s "$out/st.bin" "$out/gen.bin" ||
    { cmp "$out/st.bin" "$out/gen.bin"; fail "a register is not where ST's header has it"; }
echo "149 register addresses in 31 peripherals agree with ST's stm32f4xx.h"

# 3. built from the generated files alone, and run
"$EMBCC" $T $CF -c "$out/startup.c" -o "$out/startup.o" || fail "the generated startup"
"$EMBCC" $T $CF -c "$d/main.c" -o "$out/main.o" || fail "main.c against the generated header"
"$EMBCC" $T -T "$out/f405.ld" "$out/startup.o" "$out/main.o" -o "$out/fw.elf" ||
    fail "the generated linker script"
if command -v "$QARM" >/dev/null 2>&1; then
    sh tests/harness/qrun.sh 10 --until done "$QARM" -M netduinoplus2 \
        -nographic -kernel "$out/fw.elf" > "$out/run.txt" 2>&1
    printf 'STM32F405 via embsvd\n00005a5a 00000000 00000001 00000c24 00000025 \nticks 00000001 00000001 \ndone\n' > "$out/want.txt"
    tr -d '\r' < "$out/run.txt" | head -4 > "$out/got.txt"
    cmp -s "$out/got.txt" "$out/want.txt" ||
        { cat "$out/run.txt"; fail "the firmware did not run as written"; }
    echo "and a firmware from the generated header, startup and script runs on the STM32F405"
fi

# 4. the database, and a broken file
"$EMBSVD" "$SVD" --list > "$out/list.txt" || fail "--list"
grep -Eq '^  USART1 +0x40011000  like USART6' "$out/list.txt" || fail "--list: USART1"
"$EMBSVD" "$SVD" --show USART1 > "$out/show.txt" || fail "--show"
grep -Eq '^  0x40011008  \+0x008  BRR ' "$out/show.txt" || fail "--show: BRR"
grep -Eq '^      \[13\] +UE$' "$out/show.txt" || fail "--show: CR1.UE"
sed 's|<addressOffset>0x8</addressOffset>|<addressOffset>0x4</addressOffset>|' \
    "$SVD" > "$out/bad.svd"
if "$EMBSVD" "$out/bad.svd" --header "$out/bad.h" 2> "$out/bad.txt"; then
    fail "two registers at one offset with no alternateRegister should be refused"
fi
grep -q 'line [0-9]*: .* at offset 0x4 overlaps' "$out/bad.txt" ||
    { cat "$out/bad.txt"; fail "the refusal does not say where"; }
echo "--list and --show answer from the SVD; an overlap is refused with its line"
