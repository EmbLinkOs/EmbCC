#!/bin/sh
# -g writes call frame information (.debug_frame), and gdb unwinds with
# it. Stopped in leaf, a backtrace must reach reset through every frame
# of debug-frame/bt.c: mid, which on a Cortex-M4 with its FPU saves s16
# up with a vpush, and outer, which pushes callee-saved core registers.
# Without .debug_frame gdb read the prologue's instructions instead, and
# lost the thread right after leaf (`#1 0x00000000 in ?? ()`) -- EmbSim's
# GDB work found it.
#
# The board is EmbSim's (tests/golden/embsim-gdb.sh referees its GDB
# server against QEMU's), and the CFI itself must verify.
set -u
echo "TEST-MARKER debug-frame"
. "$(dirname "$0")/../lib.sh"

GDB=${EMBCC_GDB:-gdb}
DWD=${EMBCC_LLVM_DWARFDUMP:-llvm-dwarfdump}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
out=tests/golden/out/debug-frame
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/debug-frame/bt.c
fail=0

command -v "$GDB" >/dev/null 2>&1 || { echo "SKIP: no gdb"; exit 0; }
"$GDB" -nx -batch -ex 'set architecture arm' 2>&1 | grep -q 'architecture is set to "arm"' ||
    { echo "SKIP: $GDB has no ARM support"; exit 0; }
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }

port=$(( 33000 + ($$ % 1000) * 10 ))
simpids=
cleanup() { for p in $simpids; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

# run TAG TRIPLE BOARD OPT: build, debug, and check the backtrace
run() {
    tag=$1; t=$2; board=$3; o=$4
    "$EMBCC" --target=$t -g $o -c $src -o "$out/$tag.o" &&
    "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/$tag.o" \
        -o "$out/$tag.elf" > /dev/null 2>&1 || {
        echo "FAIL $tag: the firmware does not build"; fail=1; return; }
    if command -v "$DWD" >/dev/null 2>&1; then
        "$DWD" --debug-frame "$out/$tag.o" | grep -q ' FDE ' || {
            echo "FAIL $tag: no FDE in .debug_frame"; fail=1; }
        "$DWD" --verify "$out/$tag.o" > "$out/$tag.verify" 2>&1 || {
            echo "FAIL $tag: llvm-dwarfdump --verify rejects the DWARF"; fail=1; }
    fi
    port=$((port + 1))
    "$EMBSIM" "$out/$tag.elf" --board $board --gdb $port --gdb-wait \
        > "$out/$tag.sim" 2>&1 &
    simpids="$simpids $!"
    sleep 0.5
    timeout 60 "$GDB" -nx -batch -ex "file $out/$tag.elf" \
        -ex "target remote localhost:$port" -ex 'set backtrace past-main on' \
        -ex 'break leaf' \
        -ex 'continue' -ex 'bt' -ex 'continue' -ex 'bt' \
        -ex 'frame 2' -ex 'p $s16' -ex 'frame 3' -ex 'p $s16' -ex 'kill' \
        > "$out/$tag.gdb" 2>&1
    frames=$(grep -c '^#' "$out/$tag.gdb") || frames=0
    for fn in leaf mid outer reset; do
        n=$(grep -cE "^#[0-9]+ +(0x[0-9a-f]+ in )?$fn " "$out/$tag.gdb") || n=0
        [ "$n" -ge 2 ] || {
            echo "FAIL $tag: $fn is in $n of the 2 backtraces"
            grep '^#\|Backtrace' "$out/$tag.gdb" | head -6 | sed 's/^/     | /'
            fail=1; return; }
    done
    if grep -q '^#.* in ?? ' "$out/$tag.gdb"; then
        echo "FAIL $tag: a frame gdb could not name"
        grep '^#' "$out/$tag.gdb" | head -6 | sed 's/^/     | /'
        fail=1; return
    fi
    # s16 in the callers' frames, read back from where the vpushes put
    # it: outer keeps the constant 2.0f there across its calls, and main
    # (frame 3, or reset when main is inlined) left pi there for outer
    case $tag in m4hf-*)
        grep -q '^\$1 = 2$' "$out/$tag.gdb" &&
            grep -q '^\$2 = 3.14159274' "$out/$tag.gdb" || {
            echo "FAIL $tag: s16 in outer's and main's frames is not 2 and pi"
            grep '^\$[12]' "$out/$tag.gdb" | sed 's/^/     | /'
            fail=1; return; } ;;
    esac
    echo "  $tag: break leaf, bt reaches reset through mid and outer ($frames frames in 2 backtraces)"
}

run m4hf-O0 thumbv7em-none-eabihf mps2-an386 -O0
run m4hf-O2 thumbv7em-none-eabihf mps2-an386 -O2
run m4hf-Os thumbv7em-none-eabihf mps2-an386 -Os
run m3-O2   thumbv7m-none-eabi    lm3s6965evb -O2
[ $fail = 0 ] && echo "gdb unwinds Cortex-M frames (vpush included) by .debug_frame"
exit $fail
