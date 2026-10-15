#!/bin/sh
# embsim --board pico: the Raspberry Pi Pico's RP2040, held to its
# datasheet (no QEMU has an RP2040).
#
#  1. EmbCC builds for it from RP2040.svd (the Pico SDK's): embsvd writes
#     the header, the startup with its boot2, and the linker script that
#     puts boot2 first in flash and the vector table at 0x10000100. The
#     boot2 must be what llvm-mc assembles from embsim-pico/boot2.s, and
#     its last word the CRC-32 the bootrom checks, worked out here apart.
#  2. embsim-pico/pico.c on EmbSim: the boot as the bootrom and boot2 leave
#     it, EmbSim's ROM word for word against llvm-mc's assembly of
#     embsim-pico/rom.s, a peripheral held in reset, RESETS, the crystal's
#     startup delay, the PLL and the clock generators as the SDK's
#     clocks_init sets them (FC0 measures each), UART0 at 115200 (the
#     datasheet's divisors, 67 and 52), the atomic XOR, set and clear
#     aliases (natively and through the UART's interposer), the
#     datasheet's narrow_io_write example, the timer's 64-bit count, its
#     latch and an alarm's interrupt, the LED on GP25 blinking at 500 us,
#     GPIO_STATUS through the overrides, the pads' pulls, GP24's VBUS,
#     an edge's interrupt, the SIO's divider, spinlocks and FIFOs, and
#     the flash's JEDEC ID, unique ID, status and data through the SSI.
#     Its output must be pico.txt, the values the datasheet gives (and the
#     SDK where the datasheet quotes it); stderr the LED's changes
#     (pico-led.txt), its blinks 500 us of the timer apart at the board's
#     time, and the warnings of a peripheral read while held in reset.
#  3. The board's inputs: BOOTSEL pressed (--set), bytes on UART0's
#     receiver (--input).
#  4. A flash with no valid boot2 is refused as the bootrom refuses it; the
#     watchdog's reset ends the run when its count runs out.
#  5. With the Pico SDK at hand: its boot2_w25q080 (assembled by clang,
#     its CRC added here) boots the same program through the SSI and the
#     W25Q's status registers: QE set, the XIP in quad mode as the SDK's
#     source sets it.
#  6. With QEMU: the core. No QEMU has a Cortex-M0+, but its micro:bit's
#     M0 runs ARMv6-M: the exec corpus runs on both, from the Pico's flash
#     and boot on EmbSim, with the same output, exit status, and
#     instruction and cycle counts but for the boot's.
# RP2040.svd is looked for in $EMBREF/svd, then in the Pico SDK under
# $EMBREF/dl/rp2040; without it the test is skipped (SKIP is printed).
set -u
echo "TEST-MARKER embsim-pico"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
EMBLD=${EMBLD:-./embld}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OC=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
CLANG=${EMBCC_CLANG:-clang}
ref=${EMBREF:-$HOME/EmbRef}
sdk=${PICO_SDK_PATH:-$ref/dl/rp2040/pico-sdk}
d=tests/golden/embsim-pico
out=tests/golden/out/embsim-pico
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"

SVD=$ref/svd/RP2040.svd
[ -f "$SVD" ] || SVD=$sdk/src/rp2040/hardware_regs/RP2040.svd
[ -f "$SVD" ] || { echo "SKIP: no RP2040.svd in $ref/svd or the Pico SDK"; exit 0; }
svddir=$(dirname "$SVD")

# ---- 1. the build ---------------------------------------------------------------
T=--target=thumbv6m-none-eabi
"$EMBSVD" "$SVD" --no-cmsis --header "$out/RP2040.h" --startup "$out/startup.c" \
    --ld "$out/rp2040.ld" --flash 0x10000000:2M --ram 0x20000000:264K > /dev/null ||
    fail "embsvd"
build() {   # build OUT.elf STARTUP.c [CFLAGS...]
    o=$1; st=$2; shift 2
    "$EMBCC" $T -O2 -I"$out" -c "$st" -o "$o.startup.o" &&
    "$EMBCC" $T -O2 -I"$out" "$@" -c $d/pico.c -o "$o.o" &&
    "$EMBCC" $T -T "$out/rp2040.ld" "$o.startup.o" "$o.o" -o "$o"
}
build "$out/pico.elf" "$out/startup.c" || fail "pico.c does not build"
# boot2: the section's 256 bytes, against llvm-mc's assembly and the CRC
"$OC" -O binary -j .boot2 "$out/pico.elf" "$out/boot2.bin" || fail "no .boot2 in pico.elf"
python3 -I - "$out/boot2.bin" <<'EOF' > "$out/boot2.txt" || fail "boot2 is not 256 bytes with its CRC"
import sys
b = open(sys.argv[1], 'rb').read()
assert len(b) == 256, len(b)
c = 0xffffffff
for x in b[:252]:
    c ^= x << 24
    for _ in range(8):
        c = ((c << 1) ^ 0x04c11db7 if c & 0x80000000 else c << 1) & 0xffffffff
assert c == int.from_bytes(b[252:], 'little'), (hex(c), b[252:].hex())
print(b[:252].rstrip(b'\0').hex())
EOF
if command -v "$MC" > /dev/null 2>&1 && command -v "$OC" > /dev/null 2>&1; then
    "$MC" --triple=thumbv6m-none-eabi -filetype=obj $d/boot2.s -o "$out/b2ref.o" &&
    "$OC" -O binary "$out/b2ref.o" "$out/b2ref.bin" || fail "llvm-mc does not assemble boot2.s"
    python3 -I -c 'import sys; print(open(sys.argv[1], "rb").read().rstrip(b"\0").hex())' \
        "$out/b2ref.bin" > "$out/b2ref.txt"
    cmp -s "$out/boot2.txt" "$out/b2ref.txt" ||
        { cat "$out/boot2.txt" "$out/b2ref.txt"; fail "embsvd's boot2 is not boot2.s"; }
    echo "embsim-pico: embsvd's boot2 is boot2.s's bytes, with the bootrom's CRC-32"
else
    echo "embsim-pico: no llvm-mc: boot2's bytes are held to their CRC alone"
fi

# ---- 2. the part ------------------------------------------------------------------
run() {     # run NAME ELF [EMBSIM OPTIONS...]: NAME.txt, NAME.err, $st
    n=$1; e=$2; shift 2
    EMBSIM_SVD_PATH=/nonexistent:$svddir "$EMBSIM" "$e" --board pico \
        --max-insns 50000000 "$@" > "$out/$n.txt" 2> "$out/$n.err"
    st=$?
}
run pico "$out/pico.elf"
[ $st = 0 ] || { cat "$out/pico.txt" "$out/pico.err"; fail "pico.c exits $st"; }
grep -q '^done$' "$out/pico.txt" || { cat "$out/pico.txt" "$out/pico.err"; fail "pico.c did not finish"; }
# the ROM, against its source
if command -v "$MC" > /dev/null 2>&1; then
    "$MC" --triple=thumbv6m-none-eabi -filetype=obj $d/rom.s -o "$out/rom.o" &&
    "$OC" -O binary -j .text "$out/rom.o" "$out/rom.bin" || fail "rom.s does not assemble"
    python3 -I - "$out/rom.bin" <<'EOF' > "$out/rom-ref.txt"
import sys
b = open(sys.argv[1], 'rb').read()
for a in range(0, 0x34, 4):
    print('rom.%02x %08x' % (a, int.from_bytes(b[a:a + 4], 'little')))
EOF
    grep '^rom\.' "$out/pico.txt" > "$out/rom.txt"
    cmp -s "$out/rom.txt" "$out/rom-ref.txt" ||
        { diff "$out/rom-ref.txt" "$out/rom.txt"; fail "EmbSim's ROM is not rom.s"; }
fi
cmp -s "$out/pico.txt" $d/pico.txt ||
    { diff $d/pico.txt "$out/pico.txt"; fail "the Pico is not as the RP2040's datasheet has it (pico.txt)"; }
grep '^pico: ' "$out/pico.err" | sed 's/ at [0-9.]* ms$//' > "$out/pico-led.txt"
cmp -s "$out/pico-led.txt" $d/pico-led.txt ||
    { diff $d/pico-led.txt "$out/pico-led.txt"; fail "the LED's changes (pico-led.txt)"; }
# the three blinks: each change 500 us of the timer after the one before
grep '^pico: ' "$out/pico.err" | sed -n '1,7s/.* at \([0-9.]*\) ms$/\1/p' |
    awk 'NR > 1 { d = $1 - p; if (d < 0.4995 || d > 0.5005) bad = 1 } { p = $1 }
         END { exit bad || NR != 7 }' ||
    { grep '^pico: ' "$out/pico.err"; fail "the LED's blinks are not 500 us apart"; }
sed 's/pc 0x[0-9a-f]*/pc/' "$out/pico.err" | grep '^embsim: warning' > "$out/pico-warn.txt"
cmp -s "$out/pico-warn.txt" $d/pico-warn.txt ||
    { diff $d/pico-warn.txt "$out/pico-warn.txt"; fail "a read of a peripheral held in reset does not warn"; }
echo "embsim-pico: pico.c's $(grep -c ' ' $d/pico.txt) values as the RP2040's datasheet has them, the LED's $(grep -c . $d/pico-led.txt) changes, its blinks 500 us apart"

# ---- 3. the board's inputs ------------------------------------------------------
run bootsel "$out/pico.elf" --set BOOTSEL=pressed
grep -qx 'gpio.bootsel-in 00000000' "$out/bootsel.txt" ||
    { grep bootsel "$out/bootsel.txt"; fail "BOOTSEL pressed does not pull the QSPI chip select low"; }
grep -v '^gpio.bootsel-in' "$out/bootsel.txt" > "$out/bootsel-rest.txt"
grep -v '^gpio.bootsel-in' $d/pico.txt > "$out/pico-rest.txt"
cmp -s "$out/bootsel-rest.txt" "$out/pico-rest.txt" || fail "BOOTSEL pressed changed more than its pin"
printf 'hi there\nnot this\n' > "$out/in.txt"
run input "$out/pico.elf" --input "$out/in.txt"
grep -qx "rx 'hi there'" "$out/input.txt" ||
    { grep '^rx' "$out/input.txt"; fail "UART0's receiver does not read --input"; }
echo "embsim-pico: BOOTSEL pressed, and a line on UART0's receiver"

# ---- 4. no boot2; the watchdog ------------------------------------------------
python3 -I - "$out/startup.c" "$out/bad-startup.c" <<'EOF'
import sys
s = open(sys.argv[1]).read()
i = s.index('const unsigned char __boot2[256] = {') + len('const unsigned char __boot2[256] = {')
j = s.index('0x', i)
open(sys.argv[2], 'w').write(s[:j] + '0x01' + s[j + 4:])   # push {lr} as 0xb501
EOF
build "$out/bad.elf" "$out/bad-startup.c" || fail "the bad boot2 does not build"
run bad "$out/bad.elf" --stats
[ $st = 3 ] || { cat "$out/bad.txt" "$out/bad.err"; fail "a flash without boot2 ran ($st)"; }
grep -q "the RP2040's bootrom finds no boot2 in flash" "$out/bad.err" &&
grep -q 'no boot2 in flash: the part would wait for USB' "$out/bad.err" ||
    { cat "$out/bad.err"; fail "a flash without boot2 is not refused by name"; }
[ -s "$out/bad.txt" ] && fail "a flash without boot2 printed: $(cat "$out/bad.txt")"
build "$out/wd.elf" "$out/startup.c" -DWATCHDOG_END || fail "pico.c -DWATCHDOG_END does not build"
run wd "$out/wd.elf" --stats
[ $st = 0 ] && grep -q "the watchdog's counter reached 0 and reset the chip" "$out/wd.err" ||
    { tail -3 "$out/wd.err"; fail "the watchdog's reset does not end the run ($st)"; }
echo "embsim-pico: a flash with a bad boot2 refused as the bootrom refuses it; the watchdog's reset"

# ---- 5. the SDK's boot2 ----------------------------------------------------------
b2=$sdk/src/rp2040/boot_stage2
sdk_boot2() {
mkdir -p "$out/gen/pico"
echo '#include "boards/pico.h"' > "$out/gen/pico/config_autogen.h"
: > "$out/gen/pico/version.h"        # what the SDK's CMake generates, empty
inc="-I$b2/asminclude -I$sdk/src/rp2040/hardware_regs/include -I$sdk/src/rp2_common/hardware_base/include"
inc="$inc -I$sdk/src/common/pico_base_headers/include -I$sdk/src/boards/include"
inc="$inc -I$sdk/src/rp2040/pico_platform/include -I$sdk/src/rp2_common/pico_platform_compiler/include"
inc="$inc -I$sdk/src/rp2_common/pico_platform_sections/include -I$sdk/src/rp2_common/pico_platform_panic/include"
"$CLANG" --target=thumbv6m-none-eabi -mcpu=cortex-m0plus -DPICO_RP2040=1 $inc -I"$out/gen" \
    -c "$b2/boot2_w25q080.S" -o "$out/w25q080.o" &&
"$EMBLD" -e _stage2_boot -Ttext 0x20041f00 "$out/w25q080.o" -o "$out/w25q080.elf" &&
"$OC" -O binary -j .text "$out/w25q080.elf" "$out/w25q080.bin" ||
    fail "the SDK's boot2_w25q080.S does not build"
# its bytes, padded and with the CRC (the SDK's pad_checksum), for embsvd's
python3 -I - "$out/w25q080.bin" "$out/startup.c" "$out/sdk-startup.c" <<'EOF' || fail "boot2_w25q080 is more than 252 bytes"
import sys
b = open(sys.argv[1], 'rb').read()
assert len(b) <= 252, len(b)
b = b + bytes(252 - len(b))
c = 0xffffffff
for x in b:
    c ^= x << 24
    for _ in range(8):
        c = ((c << 1) ^ 0x04c11db7 if c & 0x80000000 else c << 1) & 0xffffffff
b += c.to_bytes(4, 'little')
s = open(sys.argv[2]).read()
k = 'const unsigned char __boot2[256] = {'
i = s.index(k) + len(k)
j = s.index('};', i)
open(sys.argv[3], 'w').write(s[:i] + ', '.join('0x%02x' % x for x in b) + s[j:])
EOF
build "$out/sdk.elf" "$out/sdk-startup.c" || fail "pico.c with the SDK's boot2 does not build"
run sdk "$out/sdk.elf"
[ $st = 0 ] || { cat "$out/sdk.txt" "$out/sdk.err"; fail "pico.c with the SDK's boot2 exits $st"; }
# boot2_w25q080: QE set in status register 2; the SSI in quad XIP, its
# continuous read mode (CTRLR0: quad, 32-bit frames, EEPROM read;
# SPI_CTRLR0: mode A0, 8 address nibbles, 4 wait cycles, no instruction,
# 2C2A); the Pico's PICO_FLASH_SPI_CLKDIV, 2
for want in 'flash.SR2 00000002' 'boot.SSI.CTRLR0 005f0300' \
            'boot.SSI.SPI_CTRLR0 a0002022' 'boot.SSI.BAUDR 00000002' \
            'boot.VTOR 10000100' 'done'; do
    grep -qx "$want" "$out/sdk.txt" || { cat "$out/sdk.txt"; fail "with the SDK's boot2: no '$want'"; }
done
echo "embsim-pico: the SDK's boot2_w25q080 through the SSI: QE set, the XIP in quad mode"
}
if [ -f "$b2/boot2_w25q080.S" ] && command -v "$CLANG" > /dev/null 2>&1; then
    sdk_boot2
else
    echo "embsim-pico: no Pico SDK or no clang: the SDK's boot2_w25q080 is not run"
fi

# ---- 6. the core: the exec corpus from the Pico's flash, against QEMU ------------
# Every tests/exec program, built once for thumbv6m with lib/libc and
# lib/rt, linked twice with embsim-pico/corpus-boot.c: for QEMU's
# micro:bit (corpus-microbit.ld) and for the Pico (corpus-pico.ld: boot2,
# the vector table at 0x10000100, the XIP flash). Output and the exit
# status are semihosting's on both, so QEMU counts each instruction once
# (tools/bench's plugin), and the counts must differ by the boot's alone:
# EmbSim's ROM and boot2 on the Pico, QEMU's doubled first blocks.
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
if ! command -v "$QA" > /dev/null 2>&1 || ! "$QA" -M help 2>/dev/null | grep -q '^microbit '; then
    echo "embsim-pico: no QEMU micro:bit: the core is not compared with QEMU's"
    exit 0
fi
. tests/harness/embsim-corpus.sh
CORP=$out/corpus CTRIPLE=thumbv6m-none-eabi CFLAGS_C= CBOOT=$d/corpus-boot.c
CEXTRA=$CORP/boot2.o QLD=$d/corpus-microbit.ld ELD=$d/corpus-pico.ld CMIN=240
QARGS="-M microbit -global nrf51-soc.sram-size=65536" EARGS="--board pico"
EMBSIM_SVD_PATH=$svddir; export EMBSIM_SVD_PATH
mkdir -p "$CORP"
awk '/__attribute__\(\(section\("\.boot2"\)/,/^};/' "$out/startup.c" > "$CORP/boot2.c"
"$EMBCC" --target=$CTRIPLE -O2 -c "$CORP/boot2.c" -o "$CORP/boot2.o" || fail "boot2.c does not build"
why=$(corpus_run); r=$?
[ $r = 2 ] && { echo "embsim-pico: $why: the core is not compared with QEMU's"; exit 0; }
[ $r = 0 ] || { echo "$why"; fail "the exec corpus on the Pico and on QEMU's micro:bit"; }
echo "embsim-pico: the exec corpus from the Pico's flash, against QEMU's micro:bit: $(cat "$CORP/summary")"
