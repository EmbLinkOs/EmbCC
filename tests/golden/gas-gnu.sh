#!/bin/sh
# The integrated assembler reads what a GNU-as startup file or RTOS port
# is written in, and makes the object clang's assembler makes from it.
#
# A real project's assembly is CMSIS's and the vendors': startup files
# that declare their handlers with a macro and `.thumb_set`, put the
# vector table in `.isr_vector` and the reset code in `.text.Reset_Handler`,
# load addresses with `ldr r0, =_sidata` from a literal pool and copy
# .data in an IT-predicated loop. tests/golden/gas-gnu/features.S has
# all of it; it is assembled by EmbCC and by clang, and the two objects
# must agree section by section -- names, types, flags and sizes, the
# instructions, the symbols and the relocations. With the real files
# beside it ($EMBREF, default ~/EmbRef: ARM's CMSIS_5 and ST's
# cmsis-device-f4), every Cortex-M startup in them is held to the same.
set -u
echo "TEST-MARKER gas-gnu"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
CLANG=${EMBCC_REF_CLANG:-clang}
command -v "$CLANG" >/dev/null 2>&1 || { echo "SKIP: no $CLANG to compare against"; exit 0; }
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
out=tests/golden/out/gas-gnu
rm -rf "$out"; mkdir -p "$out"
T=thumbv7em-none-eabi

# the parts of an object two assemblers must agree on (view.py)
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3 to compare with"; exit 0; }
view() { python3 tests/golden/gas-gnu/view.py "$1"; }

same() {    # same FILE TAG [TARGET CPU]
    t=${3:-$T}; cpu=${4:-cortex-m4}
    "$EMBCC" --target=$t -c "$1" -o "$out/$2.o" 2> "$out/$2.err" ||
        { cat "$out/$2.err"; echo "FAIL: EmbCC does not assemble $1"; exit 1; }
    "$CLANG" --target=$t -mcpu=$cpu -c "$1" -o "$out/$2.ref.o" 2> /dev/null ||
        { echo "SKIP $2: clang does not assemble it either"; return 0; }
    view "$out/$2.o" > "$out/$2.view"
    view "$out/$2.ref.o" > "$out/$2.ref.view"
    diff "$out/$2.view" "$out/$2.ref.view" > "$out/$2.diff" ||
        { head -30 "$out/$2.diff"; echo "FAIL: $2 is not the object clang makes"; exit 1; }
    return 0
}

same tests/golden/gas-gnu/features.S features
echo "features.S: macros, .irp/.rept/.if, .equ/.set/.thumb_set, sections and"
echo "  .pushsection, literal pools, relaxed branches, IT loops: clang's object"

ref=${EMBREF:-$HOME/EmbRef}
n=0
for f in "$ref"/CMSIS_5/Device/ARM/ARMCM*/Source/GCC/startup_*.S \
         "$ref"/cmsis-device-f4/Source/Templates/gcc/startup_*.s; do
    [ -f "$f" ] || continue
    case "$f" in
    # ARMv8-M Mainline and v8.1-M (stack limits, TrustZone): that target
    *ARMCM33*|*ARMCM35P*|*ARMCM55*|*ARMCM85*)
        same "$f" "real$n" thumbv8m.main-none-eabi cortex-m33 || exit 1 ;;
    # ARMv8-M Baseline has no EmbCC target yet
    *ARMCM23*) continue ;;
    *)  same "$f" "real$n" || exit 1 ;;
    esac
    n=$((n + 1))
done
if [ "$n" -gt 0 ]; then
    echo "and $n real CMSIS and STM32F4 startup files, each clang's object"
else
    echo "SKIP: no CMSIS files under $ref (git clone CMSIS_5 and cmsis-device-f4 there)"
fi
