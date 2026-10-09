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

# run TAG TRIPLE BOARD OPT [FLAGS]: build, debug, and check the backtrace
run() {
    tag=$1; t=$2; board=$3; o=$4; fl=${5:-}
    case $t in
    riscv*)
        extra=; freg=
        [ "$fl" ] && freg=fs0       # an FPU: main leaves pi in fs0
        lnk="-Ttext 0x80000000 -Tstack 0x80800000" ;;
    *)  extra=; freg=s16
        case $t in *eabihf) ;; *) freg= ;; esac
        lnk="-Ttext 0 -Tdata 0x20000000" ;;
    esac
    # shellcheck disable=SC2086
    "$EMBCC" --target=$t $fl -g $o -c $src -o "$out/$tag.o" &&
    "$EMBLD" -e reset $lnk "$out/$tag.o" $extra \
        -o "$out/$tag.elf" > /dev/null 2>&1 || {
        echo "FAIL $tag: the firmware does not build"; fail=1; return; }
    if command -v "$DWD" >/dev/null 2>&1; then
        "$DWD" --debug-frame "$out/$tag.o" | grep -q ' FDE ' || {
            echo "FAIL $tag: no FDE in .debug_frame"; fail=1; }
        "$DWD" --verify "$out/$tag.o" > "$out/$tag.verify" 2>&1 || {
            echo "FAIL $tag: llvm-dwarfdump --verify rejects the DWARF"; fail=1; }
    fi
    port=$((port + 1))
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/$tag.elf" $board --gdb $port --gdb-wait \
        > "$out/$tag.sim" 2>&1 &
    simpids="$simpids $!"
    sleep 0.5
    timeout 60 "$GDB" -nx -batch -ex "file $out/$tag.elf" \
        -ex "target remote localhost:$port" -ex 'set backtrace past-main on' \
        -ex 'break leaf' \
        -ex 'continue' -ex 'bt' -ex 'continue' -ex 'bt' \
        -ex 'frame 3' -ex "p \$${freg:-pc}" -ex 'kill' \
        > "$out/$tag.gdb" 2>&1
    frames=$(grep -c '^#' "$out/$tag.gdb") || frames=0
    for fn in leaf mid outer reset; do
        n=$(grep -cE "^#[0-9]+ +(0x[0-9a-f]+ in )?$fn " "$out/$tag.gdb") || n=0
        [ "$n" -ge 2 ] || {
            echo "FAIL $tag: $fn is in $n of the 2 backtraces"
            grep '^#\|Backtrace' "$out/$tag.gdb" | head -6 | sed 's/^/     | /'
            fail=1; return; }
    done
    # A frame gdb cannot name before reset is a broken unwind; past it is
    # only reset's own caller, which on RISC-V embld's stack stub leaves
    # as ra = 0.
    if awk '/^#/ && / in reset /{done=1} /^#0 /{done=0} !done && /^#.* in \?\? /{bad=1} END{exit !bad}' "$out/$tag.gdb"; then
        echo "FAIL $tag: a frame gdb could not name"
        grep '^#' "$out/$tag.gdb" | head -6 | sed 's/^/     | /'
        fail=1; return
    fi
    # The callee-saved float register main left pi in (frame 3: main, or
    # reset when main is inlined), read back from where the callees below
    # saved it -- by their CFI, since each saves it somewhere else.
    if [ "$freg" ]; then
        # (RV64's fs0 prints as {float = ..., double = ...}, NaN-boxed)
        grep -qE '^\$1 = (\{float = )?3\.14159274' "$out/$tag.gdb" || {
            echo "FAIL $tag: \$$freg in main's frame is not the pi main left there"
            grep '^\$1' "$out/$tag.gdb" | sed 's/^/     | /'
            fail=1; return; }
    fi
    echo "  $tag: break leaf, bt reaches reset through mid and outer ($frames frames in 2 backtraces)"
}

run m4hf-O0 thumbv7em-none-eabihf "--board mps2-an386" -O0
run m4hf-O2 thumbv7em-none-eabihf "--board mps2-an386" -O2
run m4hf-Os thumbv7em-none-eabihf "--board mps2-an386" -Os
run m3-O2   thumbv7m-none-eabi    "--board lm3s6965evb" -O2
run m0-O0   thumbv6m-none-eabi    "--board microbit --ram-size 64K" -O0
run m0-O2   thumbv6m-none-eabi    "--board microbit --ram-size 64K" -O2
if "$GDB" -nx -batch -ex 'set architecture riscv:rv64' 2>&1 | grep -q 'riscv:rv64'; then
    run rv32-O2 riscv32-unknown-elf "--board virt --ram-size 8M" -O2
    run rv64d-O0 riscv64-unknown-elf "--board virt --ram-size 8M" -O0 "-march=rv64gc -mabi=lp64d"
    run rv64d-O2 riscv64-unknown-elf "--board virt --ram-size 8M" -O2 "-march=rv64gc -mabi=lp64d"
else
    echo "  SKIP the RISC-V runs: $GDB has no riscv:rv64"
fi
[ $fail = 0 ] && echo "gdb unwinds Cortex-M and RISC-V frames (saved float registers included) by .debug_frame"
exit $fail
