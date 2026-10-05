#!/bin/sh
# Cortex-M: the busiest value gets a low register.
#
# Thumb-2's 16-bit forms reach r0-r7 only, so a pointer every load and
# store goes through costs two bytes per access in r8. The callee-saved
# registers are renamed after allocation so that the most-used one is the
# lowest (t_lowregs), and each function is generated with and without the
# rename and the shorter kept.
#
# The function is tests/exec/lowregs-notify.c's notify_isr, the shape of
# FreeRTOS's xTaskGenericNotifyFromISR, where the allocator alone puts the
# task pointer in r8. Here:
#   - no load or store goes through r8, and the task pointer's register
#     is a low one, at -Os, -O2 and on a Cortex-M4F;
#   - the default is never larger than either forced choice
#     (EMBCC_T_LOWREGS=0/1).
# The program itself runs on the boards with the exec corpus.
set -u
echo "TEST-MARKER thumb-lowregs"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
out=tests/golden/out/thumb-lowregs
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
src=tests/exec/lowregs-notify.c
fsize() { "$OBJDUMP" -t "$1" | awk '/ notify_isr$/ { print $5 }'; }
n=0
for cfg in "thumbv7m-none-eabi -Os" "thumbv7m-none-eabi -O2" \
           "thumbv7em-none-eabihf -mfpu=fpv4-sp-d16 -Os"; do
    tag=$(echo "$cfg" | tr ' =' '__')
    "$EMBCC" --target=$cfg -c "$src" -o "$out/$tag.o" || fail "$cfg: compile"
    "$OBJDUMP" -d --no-show-raw-insn "$out/$tag.o" |
        sed -n '/<notify_isr>:/,/^$/p' > "$out/$tag.dis"
    if grep -q '\[r8' "$out/$tag.dis"; then
        cat "$out/$tag.dis"; fail "$cfg: a load or store through r8"
    fi
    # the base most accesses use
    b=$(grep -o '\[r[0-9]*' "$out/$tag.dis" | sort | uniq -c | sort -rn |
        awk 'NR == 1 { print $2 }')
    case "$b" in
        "[r0"|"[r1"|"[r2"|"[r3"|"[r4"|"[r5"|"[r6"|"[r7") ;;
        *) cat "$out/$tag.dis"; fail "$cfg: the busiest base is ${b#[}, not a low register" ;;
    esac
    d=$(fsize "$out/$tag.o")
    for L in 0 1; do
        EMBCC_T_LOWREGS=$L "$EMBCC" --target=$cfg -c "$src" -o "$out/$tag-$L.o" ||
            fail "$cfg: compile with EMBCC_T_LOWREGS=$L"
        f=$(fsize "$out/$tag-$L.o")
        [ $((0x$d)) -le $((0x$f)) ] ||
            fail "$cfg: $((0x$d)) bytes by default, $((0x$f)) with EMBCC_T_LOWREGS=$L"
    done
    n=$((n + 1))
done
echo "thumb-lowregs: $n configurations keep the busiest pointer in a low register"
