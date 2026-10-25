#!/bin/sh
# _Float16 between EmbCC and another compiler, on each calling convention.
#
# A binary16 argument or result travels as the convention passes one
# (sema's ty_half_carrier): in the low half of an SSE, NEON or VFP register,
# NaN-boxed in a RISC-V FPR, sign-extended in a RISC-V integer register,
# zero-extended in an ARM one -- and on Apple's arm64 two bytes of the
# packed stack. Getting any of them wrong passes every test EmbCC compiles
# alone: a caller and a callee that agree with each other. So
# tests/golden/float16-abi/main.c and lib.c are built by EmbCC and by the
# reference compiler, and run both ways round: EmbCC calling the other's
# functions, and the other calling EmbCC's, at -O0 and -O2. The functions
# only move halves, so neither side needs a conversion routine.
#
#   darwin   EmbCC and clang on this Mac, natively (aarch64-apple-darwin)
#   x86_64   GCC's x86_64-elf objects, run on the x86-64 harness
#   aarch64  GCC's aarch64-elf objects, on the AArch64 harness
#   rv32 rv32f rv64d   clang's objects on QEMU's virt, soft-float, ilp32f
#            and lp64d
#   m4 m4hf  clang's objects on the Cortex-M4 board, soft and hard float
#
# A struct of halves is in the test where EmbCC passes one (the ARM and
# RISC-V hard-float conventions refuse it: see the C language manual).
set -u
echo "TEST-MARKER float16-abi"
EMBCC_ROOT=$PWD
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBCC=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLD=${EMBLD:-$PWD/embld}
export EMBLD
CLANG=${EMBCC_CLANG:-clang}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
Q32=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
Q64=${EMBCC_QEMU_RISCV64:-qemu-system-riscv64}
S=tests/golden/float16-abi
out=tests/golden/out/float16-abi
rm -rf "${out:?}"; mkdir -p "$out"
fails=0 ran=
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run CFG EXE: the program's exit status on CFG's machine
run() {
    case $1 in
        darwin) "$2" ;;
        x86_64) ARCH=x86_64 t_run "$2" ;;
        aarch64) ARCH=aarch64 t_run "$2" ;;
        rv32|rv32f) sh tests/harness/qrun.sh 30 "$Q32" -M virt -bios none \
                -nographic -m 8 -kernel "$2" ;;
        rv64d) sh tests/harness/qrun.sh 30 "$Q64" -M virt -bios none \
                -nographic -m 8 -kernel "$2" ;;
        m4|m4hf) sh tests/harness/qrun.sh 30 "$QA" -M mps2-an386 \
                -cpu cortex-m4 -semihosting -nographic -kernel "$2" ;;
    esac > /dev/null 2>&1
}

# EmbCC's flags, the reference compiler's command, and whether the
# convention passes a struct of halves, for CFG
cfg() {
    REF= EF= ST=-DWITH_STRUCTS RT=
    case $1 in
        darwin) EF=--target=aarch64-apple-darwin; REF="$CLANG" ;;
        x86_64) EF=--target=x86_64-elf; REF=gcc ;;
        aarch64) EF=--target=aarch64-elf; REF=gcc ;;
        # (-mno-relax: embld does not take the linker-relaxation
        # relocations clang leaves in a RISC-V object)
        rv32) EF=--target=riscv32-unknown-elf RT=riscv32-unknown-elf
              REF="$CLANG --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mno-relax" ;;
        rv32f) EF="--target=riscv32-unknown-elf -march=rv32imafc -mabi=ilp32f" ST=
               RT=riscv32-unknown-elf/ilp32f
               REF="$CLANG --target=riscv32-unknown-elf -march=rv32imafc -mabi=ilp32f -mno-relax" ;;
        rv64d) EF="--target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d" ST=
               RT=riscv64-unknown-elf/lp64d
               REF="$CLANG --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -mno-relax -mcmodel=medany" ;;
        m4) EF=--target=thumbv7em-none-eabi RT=thumbv7em-none-eabi
            REF="$CLANG --target=thumbv7em-none-eabi" ;;
        m4hf) EF=--target=thumbv7em-none-eabihf ST= RT=thumbv7em-none-eabihf
              REF="$CLANG --target=thumbv7em-none-eabihf -mfpu=fpv4-sp-d16 -mfloat-abi=hard" ;;
    esac
}

# The boards' harness: boot, output, and a driver that exits QEMU with
# main's status (as tests/golden/exec-boards.sh has them)
harness() {
    H=$out/$1; mkdir -p "$H"
    case $1 in
        rv*) for f in boot io; do
                 # shellcheck disable=SC2086
                 "$EMBCC" $EF -c tests/harness/riscv/$f.c -o "$H/$f.o" || return 1
             done
             printf '%s\n' 'int prog_main(void);' \
                 'void _exit(int r) { *(volatile unsigned *)0x100000u = r ? ((unsigned)r << 16) | 0x3333u : 0x5555u; for (;;) ; }' \
                 'int main(void) { _exit(prog_main()); }' > "$H/drv.c" ;;
        m4*) for f in boot io; do
                 # shellcheck disable=SC2086
                 "$EMBCC" $EF -DSRAM_TOP=0x20400000u -c tests/harness/thumb-m4f/$f.c \
                     -o "$H/$f.o" || return 1
             done
             printf '%s\n' 'int prog_main(void);' 'static volatile unsigned blk[2];' \
                 'void _exit(int r) { blk[0] = 0x20026u; blk[1] = (unsigned)r;' \
                 '  __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab" : : "r"(blk) : "r0", "r1", "memory");' \
                 '  for (;;) ; }' 'int main(void) { _exit(prog_main()); }' > "$H/drv.c" ;;
        *) return 0 ;;
    esac
    # shellcheck disable=SC2086
    "$EMBCC" $EF -O1 -c "$H/drv.c" -o "$H/drv.o"
}

# link CFG EXE MAIN.o LIB.o -- on a board with EmbCC's runtime, which
# EmbCC's side calls for its conversions and soft-float compares
link() {
    case $1 in
        darwin) "$CLANG" "$3" "$4" -o "$2" ;;
        x86_64|aarch64) ARCH=$1 t_link "$2" "$3" "$4" ;;
        rv*) EMBCC_RISCV_HARNESS=$out/$1 sh tests/harness/riscv/link.sh "$2" \
                 "$3" "$4" "$out/$1/drv.o" build/libc/$RT/librt.a ;;
        m4*) EMBCC_THUMB_HARNESS=$out/$1 sh tests/harness/thumb-m4f/link.sh "$2" \
                 "$3" "$4" "$out/$1/drv.o" build/libc/$RT/librt.a ;;
    esac > "$2.lerr" 2>&1
}

have() {
    case $1 in
        darwin) [ "$(uname -s)" = Darwin ] && command -v "$CLANG" >/dev/null ;;
        x86_64) command -v x86_64-elf-gcc >/dev/null ;;
        aarch64) command -v aarch64-elf-gcc >/dev/null ;;
        rv32|rv32f) command -v "$CLANG" >/dev/null && command -v "$Q32" >/dev/null ;;
        rv64d) command -v "$CLANG" >/dev/null && command -v "$Q64" >/dev/null ;;
        m4|m4hf) command -v "$CLANG" >/dev/null && command -v "$QA" >/dev/null ;;
    esac
}

for c in darwin x86_64 aarch64 rv32 rv32f rv64d m4 m4hf; do
    have $c || { echo "SKIP $c: no reference compiler or board here"; continue; }
    cfg $c
    harness $c || { bad "$c: the harness does not build"; continue; }
    case $c in rv*|m4*) PM=-Dmain=prog_main ;; *) PM= ;; esac
    # A clang without __bf16 for this target (older ones have it on fewer)
    # still referees _Float16: both sides are compiled without the __bf16
    # calls, and the run says so.
    NB=
    if [ "$REF" != gcc ] &&
       ! printf '__bf16 f(__bf16 x) { return x; }\n' |
           $REF $ST -ffreestanding -fsyntax-only -x c - 2>/dev/null; then
        NB=-DNO_BF16
        echo "  $c: the reference compiler has no __bf16 here; _Float16 alone"
    fi
    ST="$ST $NB"
    for O in -O0 -O2; do
        o=$out/$c$O
        # shellcheck disable=SC2086
        "$EMBCC" $EF $O $ST $PM -c $S/main.c -o $o-main-e.o &&
            "$EMBCC" $EF $O $ST -c $S/lib.c -o $o-lib-e.o ||
            { bad "$c $O: EmbCC does not compile it"; continue; }
        if [ "$REF" = gcc ]; then
            ARCH=$c t_gcc_c $O $ST $S/main.c -o $o-main-r.o &&
                ARCH=$c t_gcc_c $O $ST $S/lib.c -o $o-lib-r.o
        else
            # shellcheck disable=SC2086
            $REF $O $ST $PM -ffreestanding -c $S/main.c -o $o-main-r.o &&
                $REF $O $ST -ffreestanding -c $S/lib.c -o $o-lib-r.o
        fi || { bad "$c $O: the reference compiler does not compile it"; continue; }
        for w in "e r" "r e"; do
            set -- $w
            link $c $o-$1$2 $o-main-$1.o $o-lib-$2.o ||
                { bad "$c $O main=$1 lib=$2: does not link: $(head -1 $o-$1$2.lerr)"; continue; }
            run $c $o-$1$2; st=$?
            [ $st = 42 ] || bad "$c $O: main by $([ $1 = e ] && echo EmbCC || echo the reference), lib by $([ $2 = e ] && echo EmbCC || echo the reference): exit $st, want 42"
        done
    done
    ran="$ran $c"
done
[ $fails = 0 ] || exit 1
[ -n "$ran" ] || { echo "SKIP: no reference compiler for any convention"; exit 0; }
echo "float16-abi: EmbCC and the other compiler call each other's _Float16 functions on$ran"
