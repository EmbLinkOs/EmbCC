#!/bin/sh
# -g at -O2 and -Os, proven by DEBUGGING against a reference: the same
# program, built by EmbCC at -O0, -O2 and -Os and by clang at -O0, runs on
# QEMU's boards under gdb, which stops on every line and reads every
# parameter and local in scope (debug-optimized/trace.py). Every value
# EmbCC's DWARF gives must be the one clang's -O0 build shows at the same
# stop -- the same call of each function on the stack, the same line, the
# same visit of it within that call (debug-optimized/cmp.py). A call
# EmbCC inlined is a frame of its own to gdb (DW_TAG_inlined_subroutine),
# so its stops pair with clang's real call. A location list that
# names the wrong register, a value that went stale, or a variable shown
# outside its block -- the second of two `for (int i ...)` loops showing
# the first's i, a block's `s` hiding the function's own after the block
# -- is a failure. What the optimized code does not keep is <optimized
# out> and counts as unavailable, not wrong; the share available is
# printed. A local is not compared up to its declaration's line (the
# DWARF scope is its block, C's starts at the declarator), and a line
# visited a different number of times within one call (a rotated loop's
# test, which the optimized line table gives several addresses) is not
# compared in that call.
#
# clang's -O0 is the reference because a build of EmbCC's own can be
# wrong the same way at every level: before lexical blocks, -O0 and -O2
# both showed the first loop's i in the second loop, and agreed.
#
# Skips honestly without a gdb that has Python and the board's
# architecture (gdb-multiarch), clang, or the board's QEMU.
set -u
echo "TEST-MARKER debug-optimized"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
here=$EMBCC_ROOT/tests/golden/debug-optimized
out=$EMBCC_ROOT/tests/golden/out/debug-optimized
rm -rf "$out"; mkdir -p "$out"
GDB=${EMBCC_GDB:-}
if [ -z "$GDB" ]; then
    for g in gdb-multiarch gdb; do
        command -v $g >/dev/null 2>&1 &&
            $g -batch -nx -ex "python import gdb" -ex "set architecture armv7e-m" \
                >/dev/null 2>&1 && { GDB=$g; break; }
    done
fi
[ -n "$GDB" ] || { echo "skipped: no gdb with Python and ARM (gdb-multiarch)"; exit 0; }
command -v clang >/dev/null 2>&1 || { echo "skipped: no clang for the reference"; exit 0; }

fail=0 ran=0
board() {   # NAME TRIPLE HARNESS HARNESS-VAR GDB-ARCH "CLANG-FLAGS" QEMU...
    b=$1 X=$2 H=$3 HV=$4 A=$5 CF=$6; shift 6
    command -v "$1" >/dev/null 2>&1 || { echo "  $b: skipped ($1 absent)"; return; }
    $GDB -batch -nx -ex "set architecture $A" >/dev/null 2>&1 ||
        { echo "  $b: skipped ($GDB has no $A)"; return; }
    d=$out/$b; mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target=$X -c tests/harness/$H/$f.c -o "$d/$f.o" || { fail=1; return; }
    done
    for o in clang -O0 -O2 -Os; do
        if [ $o = clang ]; then
            # shellcheck disable=SC2086
            clang --target=$X $CF -O0 -g -ffreestanding -c "$here/prog.c" -o "$d/prog$o.o"
        else
            "$EMBCC" --target=$X $o -g -c "$here/prog.c" -o "$d/prog$o.o"
        fi || { echo "  $b $o: does not compile"; fail=1; continue; }
        env $HV="$d" sh tests/harness/$H/link.sh "$d/prog$o.elf" "$d/prog$o.o" \
            build/libc/$X/librt.a > "$d/$o.ld" 2>&1 ||
            { echo "  $b $o: does not link"; head -3 "$d/$o.ld"; fail=1; continue; }
        port=$((20000 + ($$ * 7 + ran) % 20000))
        "$@" -nographic -kernel "$d/prog$o.elf" -S -gdb tcp::$port > "$d/$o.qemu" 2>&1 &
        q=$!
        sleep 1
        TRACE_PORT=$port TRACE_SRC=prog.c TRACE_LINES="1-$(wc -l < "$here/prog.c")" \
        TRACE_OUT="$d/$o.jsonl" TRACE_MAX=3000 \
            $GDB -batch -nx -ex "set architecture $A" -ex "file $d/prog$o.elf" \
                -x "$here/trace.py" > "$d/$o.gdb" 2>&1
        kill $q 2>/dev/null; wait $q 2>/dev/null
        ran=$((ran + 1))
    done
    for o in -O0 -O2 -Os; do
        [ -s "$d/clang.jsonl" ] && [ -s "$d/$o.jsonl" ] ||
            { echo "  $b $o: no trace"; fail=1; continue; }
        r=$(python3 "$here/cmp.py" "$d/clang.jsonl" "$d/$o.jsonl")
        echo "  $b $o: $(echo "$r" | head -1)"
        case $(echo "$r" | head -1) in
            *", 0 DISAGREE") ;;
            *) echo "$r" | sed -n '2,12p'; fail=1 ;;
        esac
    done
}
board m3   thumbv7m-none-eabi  thumb    EMBCC_THUMB_HARNESS    armv7e-m \
    "-mcpu=cortex-m3 -mfloat-abi=soft" qemu-system-arm -M lm3s6965evb -cpu cortex-m3
board m0   thumbv6m-none-eabi  thumb-m0 EMBCC_THUMB_M0_HARNESS armv6-m \
    "-mcpu=cortex-m0 -mfloat-abi=soft" qemu-system-arm -M microbit
board rv32 riscv32-unknown-elf riscv    EMBCC_RISCV_HARNESS    riscv:rv32 \
    "-march=rv32imac -mabi=ilp32 -mno-relax" qemu-system-riscv32 -M virt -bios none -m 8
# clang's reference for MIPS is DWARF 4: gdb 17 maps no address of a
# linked MIPS image whose unit's start is a DWARF 5 .debug_addr entry
# (DW_FORM_addrx, which it does not sign-extend as it does the line
# table's KSEG0 addresses), so clang's DWARF 5 image has no lines to it
board mips mipsel-none-elf     mips     EMBCC_MIPS_HARNESS     mips:isa32r2 \
    "-mcpu=mips32r2 -msoft-float -gdwarf-4" qemu-system-mipsel -M malta -cpu 24Kc
[ $fail = 0 ] || { echo "FAIL: debug-optimized"; exit 1; }
echo "debug-optimized: every value EmbCC's DWARF gives at -O0, -O2 and -Os is clang's -O0 build's"
