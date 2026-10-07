#!/bin/sh
# The exec corpus on the embedded boards.
#
# Every tests/exec program that names its exit status (`// expect-exit:
# N`) is compiled for the target, linked with EmbCC's own libc and
# runtime, run on QEMU, and must exit N -- at -O0, -O1, -O2 and -Os, on:
#
#   m4     thumbv7em-none-eabi on the Cortex-M4F board (mps2-an386)
#   m4hf   thumbv7em-none-eabihf, the same board with the FPU and the
#          hard-float calling convention -- what an RTOS build ships
#   a7     armv7a-none-eabi, ARM (A32) state, on virt's Cortex-A15
#   rv32   riscv32-unknown-elf on virt
#   rv64   riscv64-unknown-elf on virt
#
# The corpus ran on these only by hand, and on ARMv6-M through
# thumb-v6m-exec; `make test` ran it on x86-64 and AArch64, which share
# neither the backends nor the ABI facts (a va_list that is a pointer,
# 64-bit values in register pairs) of the targets EmbCC is for. Its first
# run found a linker bug (a thread_local moved a Cortex-M image's text
# below address 0) and two -O0 miscompiles of `x--` on wide values.
#
# The harness's boot runs the constructors (.init_array, between the
# bracket symbols embld defines, as a crt0 does); a driver calls the
# program's main, renamed, and exits QEMU with its result: semihosting's
# SYS_EXIT_EXTENDED on the M4, the test device on virt. The M4's stack is
# at the top of the board's 4 MiB: big-copy.c has 240 KiB of arrays.
# A program that does not compile for a target, or does not link against
# the embedded libc, is not applicable there (NA) -- unless it compiles
# at one level and not another. Programs that cannot hold on a 32-bit
# machine are listed below with the reason.
set -u
echo "TEST-MARKER exec-boards"
. "$(dirname "$0")/../lib.sh"

QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
Q32=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
Q64=${EMBCC_QEMU_RISCV64:-qemu-system-riscv64}
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/exec-boards
rm -rf "$out"; mkdir -p "$out/p"
EMBCC=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLD=${EMBLD:-$PWD/embld}
export EMBCC EMBLD

boards=
command -v "$QA" >/dev/null 2>&1 && boards="$boards m4 m4hf a7"
command -v "$Q32" >/dev/null 2>&1 && boards="$boards rv32"
command -v "$Q64" >/dev/null 2>&1 && boards="$boards rv64"
[ -n "$boards" ] || { echo "SKIP: no qemu-system-arm or -riscv32/64"; exit 0; }

triple() {
    case $1 in
        m4) echo thumbv7em-none-eabi ;;
        m4hf) echo thumbv7em-none-eabihf ;;
        a7) echo armv7a-none-eabi ;;
        rv32) echo riscv32-unknown-elf ;;
        rv64) echo riscv64-unknown-elf ;;
    esac
}

cat > "$out/drv-m4.c" <<'EOT'
/* main's result out through semihosting SYS_EXIT_EXTENDED (the
 * harness's boot has run the constructors) */
int prog_main(void);
static volatile unsigned blk[2];
int main(void)
{
    int r = prog_main();
    blk[0] = 0x20026u;              /* ADP_Stopped_ApplicationExit */
    blk[1] = (unsigned)r;
    __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab"
                     : : "r"(blk) : "r0", "r1", "memory");
    for (;;)
        ;
}
EOT
cat > "$out/drv-a7.c" <<'EOT'
/* tests/harness/arm-a32's boot runs the constructors and ends the run
 * with main's result through semihosting SYS_EXIT_EXTENDED */
int prog_main(void);
int main(void) { return prog_main(); }
EOT
cat > "$out/drv-rv.c" <<'EOT'
/* main's result out through virt's test device: 0x5555 exits 0, and
 * 0x3333 with a code in the upper half exits with that code (the
 * harness's boot has run the constructors) */
int prog_main(void);
int main(void)
{
    int r = prog_main();
    *(volatile unsigned *)0x100000u =
        r ? ((unsigned)r << 16) | 0x3333u : 0x5555u;
    for (;;)
        ;
}
EOT

for b in $boards; do
    T=$(triple $b); L=$out/$b; mkdir -p "$L"
    { sh tools/build-rt.sh $T "$L" && sh tools/build-libc.sh $T "$L"; } \
        > "$L/build.log" 2>&1 || {
        echo "FAIL: lib/rt or lib/libc does not build for $T:"
        tail -3 "$L/build.log"; exit 1; }
    case $b in
        m4|m4hf) for f in boot io; do
                "$EMBCC" --target=$T -DSRAM_TOP=0x20400000u \
                    -c tests/harness/thumb-m4f/$f.c -o "$L/$f.o" || {
                    echo "FAIL: the M4 harness"; exit 1; }
            done
            "$EMBCC" --target=$T -O1 -c "$out/drv-m4.c" -o "$L/drv.o" ;;
        a7) for f in boot io; do
                "$EMBCC" --target=$T -O1 -c tests/harness/arm-a32/$f.c \
                    -o "$L/$f.o" || { echo "FAIL: the ARMv7-A harness"; exit 1; }
            done
            "$EMBCC" --target=$T -O1 -c "$out/drv-a7.c" -o "$L/drv.o" ;;
        rv*) for f in boot io; do
                "$EMBCC" --target=$T -c tests/harness/riscv/$f.c \
                    -o "$L/$f.o" || { echo "FAIL: the RISC-V harness"; exit 1; }
            done
            "$EMBCC" --target=$T -O1 -c "$out/drv-rv.c" -o "$L/drv.o" ;;
    esac || { echo "FAIL: the driver does not compile for $T"; exit 1; }
done

# What cannot hold on these machines, by program: the reason is the
# program's, not the compiler's. Each was built with clang for the same
# target and run on the same board, and fails there the same way (or
# shifts by the width, where any answer is undefined).
#
# On the 32-bit machines, programs written for a 64-bit `long`: they
# shift a long by 32 or more, store 0x1122334455667788L in one, or
# compare sizeof(long) with 8.
# atomics-reg.c checks its 64-bit forms on `long` (0x123456789abcdef0L);
# the M4's own atomics are thumb-atomic.sh's.
SKIP32=" atomics-reg attr-layout bit-builtins c-extras2 enum-wide-values
 ext-add global-aggregates globals gnu-attr-positions long-double longs
 sizeof-cast static-local-init strings structs u64-float "
# On the M4 only: complex.c, which clang's build fails there too.
SKIPM4=" complex "
# On the A7 only: alloca.c checks that alloca is 16-aligned, where AAPCS
# gives 8 -- it passes or not by where sp happens to be, and clang's
# build fails it at -O2 there too; and complex.c, as on the M4, whose
# result clang's build gives there too (both judged against clang in
# tests/golden/arm-a32-exec.sh).
SKIPA7=" alloca complex "
SKIP32=" $(echo $SKIP32) "

cat > "$out/one.sh" <<'EOT'
c=$1; opt=$2; b=$3; out=$4
name=$(basename "$c" .c)
case $b in
    m4|m4hf|a7|rv32) case "$SKIP32" in *" $name "*) exit 0 ;; esac ;;
esac
case $b in
    a7) case "$SKIPA7" in *" $name "*) exit 0 ;; esac ;;
esac
case $b in
    m4|m4hf) case "$SKIPM4" in *" $name "*) exit 0 ;; esac ;;
esac
expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
[ -n "$expect" ] || exit 0
case $b in
    m4) T=thumbv7em-none-eabi ;; m4hf) T=thumbv7em-none-eabihf ;;
    a7) T=armv7a-none-eabi ;;
    rv32) T=riscv32-unknown-elf ;;
    rv64) T=riscv64-unknown-elf ;;
esac
L=$out/$b; o=$out/p/$name-$b$opt
if ! "$EMBCC" --target=$T $opt -Dmain=prog_main -Ilib/libc/include \
       -c "$c" -o $o.o 2> $o.err; then
    echo "NA $name $b $opt: does not compile: $(grep -m1 error $o.err)"
    exit 0
fi
case $b in
    m4|m4hf) EMBCC_THUMB_HARNESS=$L sh tests/harness/thumb-m4f/link.sh $o.elf \
            $o.o $L/drv.o $L/libc.a $L/librt.a > $o.lerr 2>&1 ;;
    a7) EMBCC_A32_HARNESS=$L sh tests/harness/arm-a32/link.sh $o.elf \
            $o.o $L/drv.o $L/libc.a $L/librt.a > $o.lerr 2>&1 ;;
    rv*) EMBCC_RISCV_HARNESS=$L sh tests/harness/riscv/link.sh $o.elf \
            $o.o $L/drv.o $L/libc.a $L/librt.a > $o.lerr 2>&1 ;;
esac || { echo "NA $name $b $opt: does not link: $(head -1 $o.lerr)"; exit 0; }
case $b in
    m4|m4hf) sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" "$QA" \
            -M mps2-an386 -cpu cortex-m4 -semihosting -nographic \
            -kernel $o.elf > $o.txt 2>&1 ;;
    a7) sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" "$QA" \
            -M virt -cpu cortex-a15 -m 128 -semihosting -nographic \
            -monitor none -kernel $o.elf > $o.txt 2>&1 ;;
    rv32) sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" "$Q32" \
            -M virt -bios none -nographic -m 8 -kernel $o.elf > $o.txt 2>&1 ;;
    rv64) sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" "$Q64" \
            -M virt -bios none -nographic -m 8 -kernel $o.elf > $o.txt 2>&1 ;;
esac
got=$?
if [ "$got" = "$expect" ]; then
    echo "PASS $name $b $opt"
else
    echo "FAIL $name $b $opt: exit $got, want $expect"
fi
EOT
export SKIP32 SKIPM4 SKIPA7 QA Q32 Q64
for b in $boards; do
    for opt in -O0 -O1 -O2 -Os; do
        for c in tests/exec/*.c; do
            echo "$c $opt $b $out"
        done
    done
done | xargs -P "${EMBCC_JOBS:-8}" -n 4 sh "$out/one.sh" > "$out/results.txt" 2>&1
fail=0
for b in $boards; do
    for opt in -O0 -O1 -O2 -Os; do
        p=$(grep -c "^PASS .* $b $opt\$" "$out/results.txt")
        f=$(grep -c "^FAIL .* $b $opt:" "$out/results.txt")
        n=$(grep -c "^NA .* $b $opt:" "$out/results.txt")
        echo "$b $opt: $p pass, $f fail, $n not applicable"
        [ "$f" = 0 ] || fail=1
    done
done
grep '^FAIL' "$out/results.txt" | sort | head -40
# a program that compiles at some levels and not others is the
# compiler's failure, not the program's
grep '^NA .*does not compile' "$out/results.txt" | awk '{print $2, $3}' |
    sort | uniq -c | awk '$1 != 4 { print "FAIL (compiles at some levels only):", $2, $3; bad = 1 }
                          END { exit bad }' || fail=1
[ "$(grep -c '^PASS' "$out/results.txt")" -ge 1000 ] || {
    echo "FAIL: fewer than 1000 runs passed -- the harness, not the programs"
    fail=1; }
[ "$fail" = 0 ] && echo "the exec corpus exits as it says on$boards"
exit $fail
