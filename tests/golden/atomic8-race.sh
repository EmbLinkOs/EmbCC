#!/bin/sh
# Eight-byte atomics against a timer interrupt, on the Cortex-M3
# (lm3s6965evb, SysTick), the Cortex-M23's ARMv8-M Baseline code on
# mps2-an505 (SysTick) and RV32 (virt, the machine timer):
# tests/golden/atomic8/race.c, linked with the board's librt.a, at four
# levels. -icount: the interrupt lands every few dozen instructions, and at
# the same places every run, so it falls inside lib/rt/atomic8.c's
# routines thousands of times. Each line of race.c must end in 1: no
# update lost, no value torn either way, the caller's mask left as it
# was, and the interrupts still coming afterwards.
set -u
echo "TEST-MARKER atomic8-race"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
export EMBLD
out=tests/golden/out/atomic8-race
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
want="race 1
loads 1
stores 1
nested 1
ticks 1
DONE"
# board triple harness harness-variable qemu-and-machine
echo "m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -icount shift=2
m23 thumbv8m.base-none-eabi thumb-m23 EMBCC_M23_HARNESS qemu-system-arm -M mps2-an505 -cpu cortex-m33 -icount shift=2
rv32 riscv32-unknown-elf riscv EMBCC_RISCV_HARNESS qemu-system-riscv32 -M virt -bios none -m 8 -icount shift=0" |
while read -r b t h hv q m; do
    [ -n "${EMBCC_BOARDS:-}" ] && case " $EMBCC_BOARDS " in *" $b "*) ;; *) continue ;; esac
    command -v "$q" >/dev/null 2>&1 || { echo "  (SKIP $b: no $q)"; continue; }
    lib=build/libc/$t/librt.a
    [ -f "$lib" ] || fail "$b: no $lib (make rt-embedded)"
    o=$out/$b; mkdir -p "$o"
    for f in boot io; do
        "$EMBCC" --target=$t -O1 -c tests/harness/$h/$f.c -o "$o/$f.o" ||
            fail "$b: the harness's $f.c does not compile"
    done
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$t $O -c tests/golden/atomic8/race.c -o "$o/r.o" ||
            fail "$b $O: race.c does not compile"
        eval "$hv=\$o" sh tests/harness/$h/link.sh "$o/r.elf" "$o/r.o" "$lib" ||
            fail "$b $O: race.c does not link"
        # shellcheck disable=SC2086
        tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-60}" --until '^DONE' \
            "$q" $m -nographic -kernel "$o/r.elf" 2>/dev/null |
            tr -d '\r' | sed -n '1,/^DONE/p' | sed 's/ *$//' > "$o/got$O.txt"
        [ "$(cat "$o/got$O.txt")" = "$want" ] || {
            cat "$o/got$O.txt"
            fail "$b $O: an eight-byte atomic is not atomic against the interrupt"; }
    done
    echo "  $b: no update lost, no value torn, the mask restored, at four levels"
done || exit 1
