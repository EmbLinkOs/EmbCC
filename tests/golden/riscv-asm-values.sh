#!/bin/sh
# RISC-V inline asm whose operands are values in registers -- the same
# change as tests/golden/thumb-asm-values.sh, on RV32 and RV64.
#
# An "=r" output is the asm's value (a second one a continuation right
# after it), inputs are moved into their registers as one parallel move,
# a value live across an asm keeps out of exactly the registers it may
# change (its operands', its clobbers', its template's and its scratch,
# and every caller-saved one if it calls), and the inliner copies it:
#
#   1. the shape: a CSR critical section of always_inline intrinsics
#      inlines into its caller, which then touches no stack;
#   2. the values, run on QEMU's virt at every level and with the
#      allocator held to two registers: narrow outputs extended as their
#      type, an RV64 int with something else above bit 31, two outputs,
#      crossed operands, outputs to memory, "+r", values live across asm
#      that clobbers every temporary, a template that calls, and an
#      output's scratch a crossing value must not occupy.
set -u
echo "TEST-MARKER riscv-asm-values"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
d=tests/golden/riscv-asm-values
out=tests/golden/out/riscv-asm-values
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

# ---- 1. the shape --------------------------------------------------------
for x in 32 64; do
    T=riscv$x-unknown-elf
    for O in -O1 -O2 -Os; do
        "$EMBCC" --target=$T $O -c "$d/intrin.c" -o "$out/intrin.o" ||
            fail "rv$x $O: intrin.c"
        llvm-objdump -d --no-show-raw-insn "$out/intrin.o" > "$out/intrin$x$O.dis"
        for f in bump status; do
            sed -n "/<$f>:/,/^\$/p" "$out/intrin$x$O.dis" > "$out/$f.dis"
            [ -s "$out/$f.dis" ] || { cat "$out/intrin$x$O.dis"; fail "rv$x $O: no $f"; }
            if grep -Eq 'call|jal	|sp' "$out/$f.dis"; then
                cat "$out/$f.dis"
                fail "rv$x $O: $f calls an intrinsic or touches the stack"
            fi
        done
        n=$(grep -c . "$out/status.dis")
        [ "$n" -le 4 ] || { cat "$out/status.dis"; fail "rv$x $O: status is $n lines"; }
    done
done
echo "riscv-asm-values: CSR intrinsics inline, and their callers touch no stack"

# ---- 2. the values -------------------------------------------------------
H=$out/h; mkdir -p "$H"
for x in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP rv$x: no $QEMU"; continue; }
    T=riscv$x-unknown-elf
    mkdir -p "$H/$x"
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/riscv/$f.c -o "$H/$x/$f.o" || exit 1
    done
    printf 'all ok\n==END==\n' > "$out/want"
    for O in -O0 -O1 -O2 -Os; do
        for pool in "" 2; do
            # (unset, not empty, for the whole pool: an empty value is a
            # pool of none)
            if [ -n "$pool" ]; then
                EMBCC_RA_MAXPOOL=$pool "$EMBCC" --target=$T $O -c "$d/run.c" \
                    -o "$out/run.o" || fail "rv$x $O: run.c (pool $pool)"
            else
                "$EMBCC" --target=$T $O -c "$d/run.c" -o "$out/run.o" ||
                    fail "rv$x $O: run.c"
            fi
            EMBCC_RISCV_HARNESS="$H/$x" sh tests/harness/riscv/link.sh \
                "$out/run.elf" "$out/run.o" || fail "rv$x $O: link"
            sh tests/harness/riscv/run.sh "$out/run.elf" $x > "$out/run$x$O$pool.txt" 2>&1
            tr -d '\r' < "$out/run$x$O$pool.txt" | sed -n '1,/==END==/p' > "$out/cut"
            cmp -s "$out/cut" "$out/want" || {
                cat "$out/run$x$O$pool.txt"
                fail "rv$x $O${pool:+ (pool $pool)}: wrong values"; }
        done
    done
done
echo "riscv-asm-values: every output, input and crossing value is right on RV32 and RV64 at -O0, -O1, -O2 and -Os"
