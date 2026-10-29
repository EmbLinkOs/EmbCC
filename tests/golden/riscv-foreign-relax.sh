#!/bin/sh
# embld links another compiler's RISC-V objects as that compiler leaves
# them: assembled for a relaxing linker, with DWARF 5.
#
# Such an object resolves no distance the linker might change. A
# compressed branch between two labels is R_RISCV_RVC_BRANCH or
# RVC_JUMP; a jump table entry, a DWARF length, an .eh_frame range is an
# ADD of one label and a SUB of the other (in ULEB128 for DWARF 5); and an
# alignment is the WORST case of NOP padding, R_RISCV_ALIGN, which the
# linker cuts to what the layout needs. embld refused the first ("the next
# linker increment"), and skipped the last as a hint -- leaving whatever
# followed the padding misaligned.
#
# tests/golden/riscv-foreign-relax/lib.c is built by clang with its
# defaults (-mrelax, -g, which is DWARF 5); main.c by EmbCC. The image
# runs on QEMU's virt at RV32 and RV64, at -O0 and -O2: a switch through
# the jump table, and functions that asked for 64- and 8-byte alignment
# after a call (relaxable, so the padding before them is the worst case).
set -u
echo "TEST-MARKER riscv-foreign-relax"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBCC=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLD=${EMBLD:-$PWD/embld}
export EMBLD
CLANG=${EMBCC_CLANG:-clang}
Q32=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
Q64=${EMBCC_QEMU_RISCV64:-qemu-system-riscv64}
S=tests/golden/riscv-foreign-relax
out=tests/golden/out/riscv-foreign-relax
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v "$CLANG" >/dev/null 2>&1 || { echo "SKIP: no clang"; exit 0; }
"$CLANG" --target=riscv32-unknown-elf -c -x c /dev/null -o "$out/probe.o" \
    2>/dev/null || { echo "SKIP: this clang has no RISC-V target"; exit 0; }
ran=
for x in 32 64; do
    case $x in
        32) T=riscv32-unknown-elf Q=$Q32
            CF="--target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32" ;;
        64) T=riscv64-unknown-elf Q=$Q64
            CF="--target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany" ;;
    esac
    command -v "$Q" >/dev/null 2>&1 || continue
    H=$out/h$x; mkdir -p "$H"
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/riscv/$f.c -o "$H/$f.o" ||
            fail "rv$x: the harness"
    done
    printf '%s\n' 'int prog_main(void);' \
        'void _exit(int r) { *(volatile unsigned *)0x100000u = r ? ((unsigned)r << 16) | 0x3333u : 0x5555u; for (;;) ; }' \
        'int main(void) { _exit(prog_main()); }' > "$H/drv.c"
    "$EMBCC" --target=$T -O1 -c "$H/drv.c" -o "$H/drv.o" || fail "rv$x: the driver"
    for O in -O0 -O2; do
        o=$out/rv$x$O
        # shellcheck disable=SC2086
        "$CLANG" $CF $O -g -ffreestanding -c $S/lib.c -o $o-lib.o ||
            fail "rv$x $O: clang does not compile lib.c"
        llvm-readelf -r $o-lib.o 2>/dev/null | grep -q R_RISCV_ALIGN ||
            fail "rv$x $O: clang's object has no R_RISCV_ALIGN -- the test tests nothing"
        "$EMBCC" --target=$T $O -Dmain=prog_main -c $S/main.c -o $o-main.o ||
            fail "rv$x $O: main.c does not compile"
        EMBCC_RISCV_HARNESS=$H sh tests/harness/riscv/link.sh $o.elf \
            $o-main.o $o-lib.o "$H/drv.o" > $o.lerr 2>&1 ||
            fail "rv$x $O: does not link: $(head -1 $o.lerr)"
        sh tests/harness/qrun.sh 30 "$Q" -M virt -bios none -nographic -m 8 \
            -kernel $o.elf > /dev/null 2>&1
        st=$?
        [ $st = 42 ] || fail "rv$x $O: exits $st, want 42"
    done
    ran="$ran rv$x"
done
[ -n "$ran" ] || { echo "SKIP: no qemu-system-riscv32 or -riscv64"; exit 0; }
echo "riscv-foreign-relax: clang's relaxable objects link and run on$ran at -O0 and -O2"
