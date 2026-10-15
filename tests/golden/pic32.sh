#!/bin/sh
# A PIC32 part (--mcu=32MZ2048EFH100): what XC32 gives a PIC32 program, and
# EmbCC gives it the same way.
#
#  1. The device pack (DFP): named with --dfp=, a version or a family's
#     directory of versions, or else the newest MPLAB X installed under
#     ~/.mchp_packs. A small pack is made here, shaped as Microchip's are
#     (include/xc.h, include/xc-pic32m.h, include/proc/p<part>.h), so the
#     test needs none installed.
#  2. XC32's macros for the part (__32MZ2048EFH100__, __PIC32MZ__, __XC32,
#     __LANGUAGE_C__), which the pack's headers test, and in a .S file
#     __LANGUAGE_ASSEMBLY__ in __LANGUAGE_C__'s place.
#  3. EmbCC's <cp0defs.h> and builtins: _CP0_GET_STATUS() is `mfc0 $r,
#     $12, 0`, _CP0_BIS_CAUSE() a read and a write with ehb after it, and
#     the interrupt and breakpoint builtins di/ei/sdbbp; in assembly
#     _CP0_STATUS is the operand pair `$12, 0`.
#  4. address(N) on an SFR's extern declaration is the pack's processor
#     module's symbol; on a definition it is refused, as are the vector
#     attributes until EmbCC places handlers itself.
#  5. #pragma config: each word a unit sets, in its section, as XC32
#     emits it, from the pack's configuration.data.
set -u
echo "TEST-MARKER pic32"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
out=$PWD/tests/golden/out/pic32
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- a device pack, as MPLAB X installs one ------------------------------
mkpack() {          # mkpack DIR VERSION-STAMP
    mkdir -p "$1/include/proc"
    cat > "$1/include/xc.h" <<'EOF'
#ifndef __XC_H
#define __XC_H
#include <xc-pic32m.h>
#endif
EOF
    cat > "$1/include/xc-pic32m.h" <<'EOF'
#ifndef __XC_PIC32M_H
#define __XC_PIC32M_H
#if defined(__32MZ2048EFH100__)
#  include <proc/p32mz2048efh100.h>
#else
#  error Unknown processor!
#endif
#include <cp0defs.h>
#if defined (__LANGUAGE_C__) || defined (__LANGUAGE_C_PLUS_PLUS__)
#  include <sys/appio.h>
#  include <sys/l1cache.h>
#endif
#endif
EOF
    printf '#ifdef __LANGUAGE_C__\nextern volatile unsigned int LATBSET __attribute__((section("sfrs"), address(0xBF860138)));\n#endif\n#define PACK_STAMP %s\n' "$2" > "$1/include/proc/p32mz2048efh100.h"
    # two configuration words, as the pack's configuration.data has them:
    # aliases on consecutive lines, values after them, a numeric setting
    mkdir -p "$1/xc32/32MZ2048EFH100"
    printf '%s\r\n' 'Daytona Configuration Word Definitions: 0001' \
        'CWORD:BFC0FFC0:FFFFFFFF:FFFFFFFF' \
        'CSETTING:0000FFFF:lowerbootaliasconfig_DEVCFG3_USERID:' \
        'CSETTING:0000FFFF:USERID:Short name for DEVCFG3_USERID' \
        'CVALUE:00000000:00000000:Range is from 0 to 0xffff' \
        'CSETTING:01000000:FMIIEN:Ethernet RMII/MII Enable' \
        'CVALUE:00000000:OFF:RMII Enabled' \
        'CVALUE:01000000:ON:MII Enabled' \
        'CWORD:BFC0FFC4:FFFFFFFF:FFFFFFFF' \
        'CSETTING:00000007:FPLLIDIV:System PLL Input Divider' \
        'CVALUE:00000000:DIV_1:Divide by 1' \
        'CVALUE:00000002:DIV_3:Divide by 3' \
        > "$1/xc32/32MZ2048EFH100/configuration.data"
}
mkpack "$out/pack" 1
P="--target=mipsel-none-elf --mcu=32MZ2048EFH100"

# ---- 1. finding the pack ----------------------------------------------------
cat > "$out/a.c" <<'EOF'
#include <xc.h>
#include <sys/attribs.h>
#include <sys/kmem.h>
#include <sys/types.h>
#if !defined(__PIC32MZ__) || !defined(__XC32) || __XC32_VERSION < 2490
#error not XC32's macros for the part
#endif
int stamp = PACK_STAMP;
unsigned status(void) { return _CP0_GET_STATUS(); }
unsigned soft0(void) { return _CP0_BIS_CAUSE(1u << 8); }
unsigned off(void) { return __builtin_disable_interrupts(); }
void on(void) { __builtin_enable_interrupts(); }
void stop(void) { __builtin_software_breakpoint(); }
void clear(void) { _CP0_SET_STATUS(0); }
void led(void) { LATBSET = 1u << 3; }
unsigned long phys(void *p) { return KVA_TO_PA(p); }
ssize_t n = -1;
EOF
# shellcheck disable=SC2086
"$EMBCC" $P --dfp="$out/pack" -O2 -c "$out/a.c" -o "$out/a.o" ||
    fail "a PIC32 program does not compile against --dfp="
mkdir -p "$out/fam/1.2.3" "$out/fam/1.10.0" "$out/home/.mchp_packs/Microchip/PIC32MZ-EF_DFP"
mkpack "$out/fam/1.2.3" 2
mkpack "$out/fam/1.10.0" 3
# shellcheck disable=SC2086
"$EMBCC" $P --dfp="$out/fam" -E "$out/a.c" | grep -q 'stamp = 3' ||
    fail "--dfp= of a family's directory is not its newest version (1.10.0)"
mkpack "$out/home/.mchp_packs/Microchip/PIC32MZ-EF_DFP/2.0.1" 4
# shellcheck disable=SC2086
HOME=$out/home "$EMBCC" $P -E "$out/a.c" | grep -q 'stamp = 4' ||
    fail "the pack under ~/.mchp_packs is not found"
# shellcheck disable=SC2086
HOME=$out/nowhere "$EMBCC" $P -c "$out/a.c" -o "$out/x.o" 2> "$out/nopack.err" &&
    fail "a PIC32 part with no pack compiled"
grep -q 'no device pack for the 32MZ2048EFH100' "$out/nopack.err" ||
    { cat "$out/nopack.err"; fail "a missing pack is not named"; }
echo "pic32: the pack by --dfp= (a version, or a family's newest), or from ~/.mchp_packs"

# ---- 3. the coprocessor 0 and the builtins ----------------------------------
llvm-objdump -d --no-show-raw-insn "$out/a.o" > "$out/a.dis"
for f in status soft0 off on stop clear; do
    sed -n "/<$f>:/,/^$/p" "$out/a.dis" > "$out/$f.dis"
done
grep -q 'mfc0.*\$12, 0x0' "$out/status.dis" || { cat "$out/status.dis"; fail "_CP0_GET_STATUS is not mfc0 \$12, 0"; }
grep -q 'mfc0.*\$13, 0x0' "$out/soft0.dis" && grep -q 'mtc0.*\$13, 0x0' "$out/soft0.dis" &&
    grep -q 'ehb' "$out/soft0.dis" || { cat "$out/soft0.dis"; fail "_CP0_BIS_CAUSE is not a read, a write and ehb"; }
grep -q '	di	' "$out/off.dis" || { cat "$out/off.dis"; fail "__builtin_disable_interrupts is not di"; }
grep -q '	ei	' "$out/on.dis" || { cat "$out/on.dis"; fail "__builtin_enable_interrupts is not ei"; }
grep -q 'sdbbp' "$out/stop.dis" || { cat "$out/stop.dis"; fail "__builtin_software_breakpoint is not sdbbp"; }
grep -q 'mtc0.*\$zero, \$12' "$out/clear.dis" || { cat "$out/clear.dis"; fail "%z of the constant 0 is not \$zero"; }
# the debug and shadow-set instructions XC32's headers write: deret and
# sdbbp as llvm encodes them, rdpgpr and wrpgpr (which llvm has not) as
# the MIPS32 architecture manual lays out COP0's rs 01010 and 01110
printf '\tderet\n\tsdbbp 5\n\trdpgpr $2, $29\n\twrpgpr $29, $3\n' > "$out/sys.s"
"$EMBCC" --target=mipsel-none-elf -c "$out/sys.s" -o "$out/sys.o" || fail "sys.s"
got=$(llvm-objdump -d "$out/sys.o" | awk '/^ +[0-9a-f]+:/ { print $2 $3 $4 $5 }' | tr '\n' ' ')
[ "$got" = "1f000042 7f010070 00105d41 00e8c341 " ] ||
    fail "deret/sdbbp/rdpgpr/wrpgpr encode as '$got'"
llvm-objdump -r "$out/a.o" | grep -q 'R_MIPS_HI16.*LATBSET' ||
    fail "an SFR is not a reference to the pack's symbol"
echo "pic32: _CP0_GET_/_BIS_ are mfc0/mtc0 with ehb, %z0 of 0 \$zero, the builtins di, ei and sdbbp, deret/rdpgpr/wrpgpr encoded, an SFR the pack's symbol"

# ---- 2. assembly ------------------------------------------------------------
cat > "$out/s.S" <<'EOF'
#include <xc.h>
#ifndef __LANGUAGE_ASSEMBLY__
#error no __LANGUAGE_ASSEMBLY__
#endif
#ifdef __LANGUAGE_C__
#error __LANGUAGE_C__ in assembly
#endif
    .text
    .globl rd
rd:
    mfc0 $2, _CP0_EPC
    mtc0 $2, _CP0_STATUS
    ehb
    jr $31
    nop
EOF
# shellcheck disable=SC2086
"$EMBCC" $P --dfp="$out/pack" -c "$out/s.S" -o "$out/s.o" ||
    fail "a PIC32 .S does not assemble"
llvm-objdump -d --no-show-raw-insn "$out/s.o" | grep -q 'mfc0.*\$14, 0x0' ||
    fail "_CP0_EPC in assembly is not \$14, 0"
# a C file after the .S on the same command line still sees C's macros
# shellcheck disable=SC2086
(cd "$out" && "$EMBCC" $P --dfp="$out/pack" -c s.S a.c 2> sc.err) ||
    { cat "$out/sc.err"; fail "a C file after a .S lost __LANGUAGE_C__"; }
echo "pic32: a .S sees __LANGUAGE_ASSEMBLY__ and _CP0_EPC as \$14, 0; a C file after it, C's"

# ---- 5. #pragma config ---------------------------------------------------------
# each word a unit sets: its default, the settings' fields replaced, one
# const object in .config_<address>, which the pack's linker script places
# (ThermoV2's 44 settings give XC32's six words bit for bit)
printf '#pragma config USERID = 0x1234, FMIIEN = OFF\n#pragma config FPLLIDIV = DIV_3\nint x;\n' > "$out/cfg.c"
# shellcheck disable=SC2086
"$EMBCC" $P --dfp="$out/pack" -c "$out/cfg.c" -o "$out/cfg.o" || fail "#pragma config"
for w in "BFC0FFC0 feff1234" "BFC0FFC4 fffffffa"; do
    set -- $w
    llvm-objcopy -O binary --only-section=".config_$1" "$out/cfg.o" "$out/w.bin" ||
        fail "no section .config_$1"
    got=$(od -An -tx4 "$out/w.bin" | tr -d ' ')
    [ "$got" = "$2" ] || fail ".config_$1 holds $got, not $2"
done
cfgbad() {          # cfgbad NAME PRAGMA WANT
    printf '#pragma config %s\n' "$2" > "$out/$1.c"
    # shellcheck disable=SC2086
    "$EMBCC" $P --dfp="$out/pack" -c "$out/$1.c" -o "$out/$1.o" 2> "$out/$1.err" &&
        fail "#pragma config $2 was accepted"
    grep -q "$3" "$out/$1.err" || { cat "$out/$1.err"; fail "#pragma config $2: no reason"; }
}
cfgbad nosuch 'NOSUCH = ON' 'no setting named'
cfgbad badval 'FPLLIDIV = DIV_2' 'FPLLIDIV takes DIV_1, DIV_3'
cfgbad toobig 'USERID = 0x10000' "does not fit the setting's field"
printf '#pragma config FNOSC = SPLL\n' > "$out/nopart.c"
"$EMBCC" --target=mipsel-none-elf -c "$out/nopart.c" -o "$out/nopart.o" 2> "$out/nopart.err" &&
    fail "#pragma config without a part was accepted"
grep -q 'needs the part' "$out/nopart.err" || { cat "$out/nopart.err"; fail "#pragma config without a part: no reason"; }
echo "pic32: #pragma config sets its words' fields (a value or a number), and names what it refuses"

# ---- 4. attributes ------------------------------------------------------------
refuse() {          # refuse NAME SOURCE WANT
    printf '%s\n' "$2" > "$out/$1.c"
    # shellcheck disable=SC2086
    "$EMBCC" $P --dfp="$out/pack" -c "$out/$1.c" -o "$out/$1.o" 2> "$out/$1.err" &&
        fail "$1 was accepted"
    grep -q "$3" "$out/$1.err" || { cat "$out/$1.err"; fail "$1 is not refused by name"; }
}
refuse def-at 'volatile unsigned x __attribute__((address(0xA0001000)));' \
    "address)) on the definition of 'x' is not supported"
refuse isr '#include <sys/attribs.h>
void __ISR(9, IPL1SOFT) h(void) { }' "vector)) is not supported"
refuse coherent 'int __attribute__((coherent)) buf[4];' "coherent)) is not supported"
printf 'void __attribute__((nomips16, nomicromips)) f(void) { }\n' > "$out/m16.c"
# shellcheck disable=SC2086
"$EMBCC" $P --dfp="$out/pack" -Wattributes -c "$out/m16.c" -o "$out/m16.o" 2> "$out/m16.err" &&
    [ ! -s "$out/m16.err" ] || { cat "$out/m16.err"; fail "nomips16 is not a silent no-op"; }
echo "pic32: address on a definition, __ISR and coherent refused by name; nomips16 changes nothing"
