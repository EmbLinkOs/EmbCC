#!/bin/sh
# embsim-riscv: EmbSim's RISC-V core (tools/embsim/riscv.c), refereed by
# QEMU's virt board.
#
#  1. The exec corpus on RV32 and RV64, soft- and hard-float: every
#     tests/exec program is built with lib/libc and lib/rt for the ABI
#     and run on qemu-system-riscv32/64 -M virt and on EmbSim's virt.
#     The output, the exit status (the test device's), the instruction
#     count and the estimated cycles (tools/bench/cost.h, through QEMU's
#     counting plugin tools/bench/icount.c) must be the same:
#       rv32    riscv32-unknown-elf, ilp32 (rv32imac)      -O0 -O2 -Os
#       rv32d   -march=rv32imafdc -mabi=ilp32d              -O0 -O2
#       rv64    riscv64-unknown-elf, lp64 (rv64imac)        -O0 -O2
#       rv64d   -march=rv64gc -mabi=lp64d                   -O0 -O2 -Os
#     QEMU leaves the guest running while its main loop takes the test
#     device's exit, so its total is late by a varying amount; the plugin
#     is given the address of the driver's last function (stop=), and its
#     counts are those at the first block that starts there. That
#     function stores to the test device and loops: QEMU's block holds
#     the loop's jump, which EmbSim -- ending the run at the store -- does
#     not run, so QEMU's count is one instruction and one cycle more. It
#     is measured with a main that only returns, and every program must
#     show exactly that difference. A program QEMU cannot finish is not
#     applicable.
#  2. The ends of a run (no QEMU needed): the test device's pass and fail,
#     a loop nothing can interrupt, a WFI that nothing can wake, a trap
#     with nowhere to go, --max-insns.
#  3. Instruction edges (tests/golden/embsim/rv-isa.c, built by clang
#     for each of RV32 and RV64): M's division by zero and overflow and
#     its high multiplies, the compressed forms, misaligned loads and
#     stores, LR/SC and the AMOs, the counters and the CSRs, traps and
#     their CSRs, the CLINT's timer interrupt waking a WFI, and F and D:
#     every rounding mode, fflags after each operation, NaN-boxing, the
#     canonical NaN, minimumNumber, the conversions' saturation, fused
#     multiply-add. It prints the results; EmbSim's output must be
#     QEMU's, and the record's (tests/golden/embsim/rv-isa-*.txt).
set -u
echo "TEST-MARKER embsim-riscv"
. "$(dirname "$0")/../lib.sh"

Q32=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
Q64=${EMBCC_QEMU_RISCV64:-qemu-system-riscv64}
NM=${EMBCC_LLVM_NM:-llvm-nm}
EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
export EMBCC EMBLD EMBSIM NM
out=tests/golden/out/embsim-riscv
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

have_qemu=0
command -v "$Q32" >/dev/null 2>&1 && command -v "$Q64" >/dev/null 2>&1 &&
    have_qemu=1
plug=
if [ $have_qemu = 1 ] && command -v "$NM" >/dev/null 2>&1; then
    inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    p=$PWD/$out/icount.so
    { cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -undefined dynamic_lookup -o "$p" tools/bench/icount.c ||
      cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -o "$p" tools/bench/icount.c; } 2>/dev/null && plug=$p
fi
[ $have_qemu = 0 ] || [ -n "$plug" ] ||
    echo "note: QEMU's counting plugin (or $NM) is missing; counts are not compared"

# ---- 2. the ends of a run ------------------------------------------------
e=$out/ends; mkdir -p "$e"
cat > "$e/ends.c" <<'EOF'
/* MODE 0: the test device's fail with 77; 1: an illegal instruction (a
 * write to the read-only cycle) with mtvec 0, where nothing can be
 * fetched: a lockup; 2: a loop with interrupts off; 3: WFI with nothing
 * enabled; 4: the timer's interrupt wakes WFI, and its handler exits
 * with mcause's code; 5: prints, then loops for ever printing */
#define UART (*(volatile unsigned char *)0x10000000u)
#define TEST (*(volatile unsigned *)0x100000u)
static void puts_(const char *s) { while (*s) UART = (unsigned char)*s++; }
__attribute__((interrupt("machine"), aligned(4))) static void handler(void)
{
    unsigned long c;
    __asm__ volatile("csrr %0, mcause" : "=r"(c));
    TEST = (unsigned)((c & 0xff) << 16) | 0x3333u;
}
int main(void)
{
    puts_("ready\n");
#if MODE == 0
    TEST = 77u << 16 | 0x3333u;
#elif MODE == 1
    __asm__ volatile("csrw cycle, zero");
#elif MODE == 2
    for (;;)
        ;
#elif MODE == 3
    __asm__ volatile("wfi");
    puts_("woke\n");
#elif MODE == 4
    volatile unsigned long long *mtimecmp = (void *)0x2004000u;
    volatile unsigned long long *mtime = (void *)0x200bff8u;
    __asm__ volatile("csrw mtvec, %0" : : "r"(handler));
    *mtimecmp = *mtime + 5000;
    __asm__ volatile("csrs mie, %0" : : "r"(1u << 7));
    __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));
    for (;;)
        __asm__ volatile("wfi");
#else
    for (volatile int i = 0;; i++)
        if (i == 1000)
            puts_("==MARK==\n");
#endif
    return 0;
}
EOF
for m in 0 1 2 3 4 5; do
    "$EMBCC" --target=riscv32-unknown-elf -O1 -DMODE=$m -c "$e/ends.c" -o "$e/e$m.o" &&
    "$EMBCC" --target=riscv32-unknown-elf -c tests/harness/riscv/boot.c -o "$e/boot.o" &&
    "$EMBLD" -e _start -Ttext 0x80000000 -Tstack 0x80800000 "$e/boot.o" \
        "$e/e$m.o" -o "$e/e$m.elf" || fail "the end-of-run images do not build"
done
"$EMBSIM" "$e/e0.elf" --board virt --max-insns 1000000 > "$e/0.out" 2>&1; st=$?
[ $st = 77 ] && grep -q ready "$e/0.out" || { cat "$e/0.out"; fail "the test device's fail (77) gave status $st"; }
"$EMBSIM" "$e/e1.elf" --board virt --max-insns 1000000 > "$e/1.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'lockup' "$e/1.out" || { cat "$e/1.out"; fail "a trap with no vector is not a lockup (status $st)"; }
"$EMBSIM" "$e/e2.elf" --board virt --stats --max-insns 1000000 > "$e/2.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'nothing can interrupt' "$e/2.out" || { cat "$e/2.out"; fail "an idle loop does not end the run (status $st)"; }
"$EMBSIM" "$e/e3.elf" --board virt --stats --max-insns 1000000 > "$e/3.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'waiting for an interrupt' "$e/3.out" &&
    ! grep -q woke "$e/3.out" || { cat "$e/3.out"; fail "a WFI nothing can wake does not end the run (status $st)"; }
"$EMBSIM" "$e/e4.elf" --board virt --count "$e/4.count" --max-insns 1000000 > "$e/4.out" 2>&1; st=$?
[ $st = 7 ] || { cat "$e/4.out"; fail "the timer's interrupt did not wake WFI into its handler (status $st, not mcause 7)"; }
[ "$(sed -n 2p "$e/4.count")" -ge 5000 ] && [ "$(sed -n 1p "$e/4.count")" -lt 1000 ] ||
    fail "WFI did not skip ahead to mtimecmp: $(tr '\n' ' ' < "$e/4.count")"
"$EMBSIM" "$e/e5.elf" --board virt --until '==MARK==' --max-insns 10000000 > "$e/5.out" 2>&1; st=$?
[ $st = 0 ] && [ "$(grep -c MARK "$e/5.out")" = 1 ] || fail "--until does not stop at the first ==MARK== (status $st)"
"$EMBSIM" "$e/e5.elf" --board virt --max-insns 500 > "$e/6.out" 2>&1; st=$?
[ $st = 4 ] && grep -q '500 instructions' "$e/6.out" || { cat "$e/6.out"; fail "--max-insns 500: status $st"; }
echo "embsim-riscv: the test device's status, a lockup, an idle loop, a WFI nothing wakes, the CLINT's timer waking WFI, --until and --max-insns end a run as documented"

# ---- 3. instruction edges ----------------------------------------------------
x=$out/isa; mkdir -p "$x"
CLANG=${EMBCC_REF_CLANG:-clang}
if command -v "$CLANG" >/dev/null 2>&1; then
    for w in 32 64; do
        "$CLANG" --target=riscv$w-unknown-elf -march=rv${w}gc \
            -mabi=$([ $w = 32 ] && echo ilp32d || echo lp64d) -mcmodel=medany \
            -O1 -ffreestanding -fno-builtin -nostdlib -mno-relax \
            -c tests/golden/embsim/rv-isa.c -o "$x/isa$w.o" 2>"$x/isa$w.err" ||
            { cat "$x/isa$w.err"; fail "rv-isa.c does not compile for RV$w"; }
        "$EMBLD" -e _start -Ttext 0x80000000 -Tstack 0x80800000 \
            "$x/isa$w.o" -o "$x/isa$w.elf" || fail "rv-isa.c does not link for RV$w"
        "$EMBSIM" "$x/isa$w.elf" --board virt --max-insns 200000000 > "$x/isa$w.sim" 2>&1; st=$?
        [ $st = 0 ] || { tail -5 "$x/isa$w.sim"; fail "rv-isa.c on RV$w: EmbSim exits $st"; }
        cmp -s "$x/isa$w.sim" tests/golden/embsim/rv-isa-$w.txt || {
            diff tests/golden/embsim/rv-isa-$w.txt "$x/isa$w.sim" | head -20
            fail "rv-isa.c on RV$w: EmbSim's output is not rv-isa-$w.txt"; }
        if [ $have_qemu = 1 ]; then
            q=$Q32; [ $w = 64 ] && q=$Q64
            sh tests/harness/qrun.sh 20 "$q" -M virt -bios none -nographic \
                -m 8 -kernel "$x/isa$w.elf" > "$x/isa$w.qemu" 2>/dev/null
            cmp -s "$x/isa$w.qemu" "$x/isa$w.sim" || {
                diff "$x/isa$w.qemu" "$x/isa$w.sim" | head -20
                fail "rv-isa.c on RV$w: EmbSim's output is not QEMU's"; }
        fi
        echo "embsim-riscv: RV$w's instruction edges ($(wc -l < "$x/isa$w.sim" | tr -d ' ') results) as QEMU and the record have them"
    done
else
    echo "embsim-riscv: no clang for RISC-V: the instruction-edge program was not built"
fi

# ---- 1. the exec corpus -----------------------------------------------------
# TAG ABI-TAG TRIPLE FLAGS VARIANT OPT XLEN
configs="rv32-O0|rv32|riscv32-unknown-elf||riscv32-unknown-elf|-O0|32
rv32-O2|rv32|riscv32-unknown-elf||riscv32-unknown-elf|-O2|32
rv32-Os|rv32|riscv32-unknown-elf||riscv32-unknown-elf|-Os|32
rv32d-O0|rv32d|riscv32-unknown-elf|-march=rv32imafdc -mabi=ilp32d|riscv32-unknown-elf/ilp32d|-O0|32
rv32d-O2|rv32d|riscv32-unknown-elf|-march=rv32imafdc -mabi=ilp32d|riscv32-unknown-elf/ilp32d|-O2|32
rv64-O0|rv64|riscv64-unknown-elf||riscv64-unknown-elf|-O0|64
rv64-O2|rv64|riscv64-unknown-elf||riscv64-unknown-elf|-O2|64
rv64d-O0|rv64d|riscv64-unknown-elf|-march=rv64gc -mabi=lp64d|riscv64-unknown-elf/lp64d|-O0|64
rv64d-O2|rv64d|riscv64-unknown-elf|-march=rv64gc -mabi=lp64d|riscv64-unknown-elf/lp64d|-O2|64
rv64d-Os|rv64d|riscv64-unknown-elf|-march=rv64gc -mabi=lp64d|riscv64-unknown-elf/lp64d|-Os|64"

# the driver: main's result to the test device, from a function of its
# own -- the plugin's stop address
cat > "$out/drv.c" <<'EOT'
int prog_main(void);
void __embsim_end(unsigned v);
int main(void)
{
    int r = prog_main();
    __embsim_end(r ? ((unsigned)r << 16) | 0x3333u : 0x5555u);
    return 0;
}
EOT
cat > "$out/end.c" <<'EOT'
void __embsim_end(unsigned v)
{
    *(volatile unsigned *)0x100000u = v;
    for (;;)
        ;
}
EOT

# one.sh TAG NAME OUT: build one program and run it on both; one line
cat > "$out/one.sh" <<'EOT'
tag=$1; c=$2; out=$3
. "$out/$tag/cfg"
L=$out/$ABI
n=$(basename "$c" .c); o=$out/$tag/p/$n
# shellcheck disable=SC2086
if ! "$EMBCC" --target=$T $F $OPT -Dmain=prog_main -Ilib/libc/include -c "$c" -o $o.o 2>/dev/null ||
   ! EMBCC_RISCV_HARNESS=$L sh tests/harness/riscv/link.sh $o.elf $o.o $L/drv.o \
       $L/end.o $L/libc.a $L/librt.a > /dev/null 2>&1; then
    echo "NA $tag $n"; exit 0
fi
# a budget, so a simulator that loops fails here instead of hanging the
# suite
"$EMBSIM" $o.elf --board virt --ram-size 8M --max-insns 4000000000 \
    --count $o.en > $o.eout 2> $o.eerr; es=$?
if [ "$QEMU" = - ]; then
    echo "SIM $tag $n $es"; exit 0
fi
pl=
if [ -n "$PLUG" ]; then
    stop=$("$NM" $o.elf | sed -n 's/^0*\([0-9a-f]*\) T __embsim_end$/0x\1/p')
    pl="-plugin $PLUG,out=$o.qn,stop=$stop"
fi
# shellcheck disable=SC2086
sh tests/harness/qrun.sh 30 "$QEMU" -M virt -bios none -nographic -m 8 $pl \
    -kernel $o.elf > $o.qout 2> /dev/null; qs=$?
# killed at the timeout: 137 (a program's own status can be anything up
# to 255, and the test device's code is 16 bits, so only the plugin's
# file, written at QEMU's exit, tells a 137 from the timeout)
if [ $qs = 137 ] && { [ -z "$PLUG" ] || [ ! -s $o.qn ]; }; then
    echo "NA $tag $n (QEMU did not finish)"; exit 0
fi
if [ $qs != $es ] || ! cmp -s $o.qout $o.eout; then
    echo "DIFF $tag $n: QEMU exits $qs, EmbSim $es$(cmp -s $o.qout $o.eout || echo ', and the output differs')$(head -c 200 $o.eerr | tr '\n' ' ')"
    exit 0
fi
if [ -s "$o.qn" ]; then
    qi=$(sed -n 1p $o.qn); qc=$(sed -n 2p $o.qn)
    ei=$(sed -n 1p $o.en); ec=$(sed -n 2p $o.en)
    echo "COUNT $tag $n $((qi - ei)) $((qc - ec)) $ei"
else
    echo "SAME $tag $n"
fi
EOT

# the runtime, the harness and the driver for each ABI
for abi in rv32 rv32d rv64 rv64d; do
    line=$(echo "$configs" | grep "^[^|]*|$abi|" | head -1)
    IFS='|' read tag a T F V OPT X <<EOF
$line
EOF
    d=$out/$abi; mkdir -p "$d"
    { sh tools/build-rt.sh "$V" "$d" && sh tools/build-libc.sh "$V" "$d"; } \
        > "$d/build.log" 2>&1 || fail "$abi: lib/rt or lib/libc does not build"
    for f in boot io; do
        # shellcheck disable=SC2086
        "$EMBCC" --target=$T $F -c tests/harness/riscv/$f.c -o "$d/$f.o" ||
            fail "$abi: the harness does not compile"
    done
    for f in drv end; do
        # shellcheck disable=SC2086
        "$EMBCC" --target=$T $F -O1 -c "$out/$f.c" -o "$d/$f.o" ||
            fail "$abi: the driver does not compile"
    done
done
tags=
echo "$configs" | while IFS='|' read tag abi T F V OPT X; do
    d=$out/$tag; mkdir -p "$d/p"
    q=$Q32; [ $X = 64 ] && q=$Q64
    [ $have_qemu = 1 ] || q=-
    {
        echo "T=$T; F='$F'; OPT=$OPT; ABI=$abi"
        echo "QEMU=$q; PLUG=$plug"
    } > "$d/cfg"
    # the end's own difference in QEMU's count: a main that returns
    printf 'int main(void) { return 0; }\n' > "$d/empty.c"
    sh "$out/one.sh" $tag "$d/empty.c" "$out" > "$d/empty.result"
done || exit 1
tags=$(echo "$configs" | cut -d'|' -f1)

for tag in $tags; do
    for c in tests/exec/*.c; do echo "$tag $c $out"; done
done | xargs -P "${EMBCC_JOBS:-8}" -n 3 sh "$out/one.sh" > "$out/results.txt" 2>&1

total=0
for tag in $tags; do
    r=$out/results.txt
    base=$(cat "$out/$tag/empty.result")
    case $base in
        COUNT*) set -- $base; bi=$4; bc=$5 ;;
        SAME*|SIM*) bi=; bc= ;;
        *) fail "$tag: a main that returns does not run: $base" ;;
    esac
    grep "^DIFF $tag " "$r" && fail "$tag: EmbSim and QEMU disagree"
    if [ -n "$bi" ]; then
        bad=$(grep "^COUNT $tag " "$r" | awk -v i="$bi" -v c="$bc" '$4 != i || $5 != c')
        [ -z "$bad" ] || { echo "$bad" | head -5
            fail "$tag: QEMU's count differs from EmbSim's by other than the end's $bi instruction and $bc cycle"; }
    fi
    same=$(grep -c "^\(COUNT\|SAME\) $tag " "$r")
    sim=$(grep -c "^SIM $tag " "$r")
    na=$(grep -c "^NA $tag " "$r")
    insns=$(grep "^COUNT $tag " "$r" | awk '{ s += $6 } END { print s + 0 }')
    if [ $sim -gt 0 ]; then
        echo "embsim-riscv: $tag: $sim programs ran (no QEMU to compare with); $na not applicable"
    elif [ -n "$bi" ]; then
        echo "embsim-riscv: $tag: $same programs as QEMU, to the instruction and the cycle ($insns instructions); $na not applicable"
    else
        echo "embsim-riscv: $tag: $same programs as QEMU, output and exit status; $na not applicable"
    fi
    total=$((total + same))
done
[ $have_qemu = 0 ] || [ $total -ge 2000 ] || fail "only $total programs were compared"
