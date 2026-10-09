#!/bin/sh
# embsim: the Cortex-M simulator (tools/embsim), refereed by QEMU.
#
#  1. The exec corpus on five cores: every tests/exec program is built
#     with lib/libc and lib/rt and run on QEMU's model of a board and on
#     EmbSim's. The output and the exit status must be the same. Where
#     QEMU's instruction count can be had exactly (tools/bench's plugin),
#     the count and the estimated cycles must be too, both from
#     tools/bench/cost.h:
#       m3    thumbv7m on the lm3s6965evb, -O0
#       m4    thumbv7em soft float on the mps2-an386, -Os
#       m4hf  thumbv7em-eabihf (FPv4-SP) on the mps2-an386, -O2
#       m7    thumbv7em-eabihf, -mcpu=cortex-m7 (FPv5 double) on the
#             mps2-an500, -O2
#       m0    thumbv6m on the micro:bit with 64 KiB of SRAM, -O2
#     The plugin adds up each translation block when it starts. QEMU
#     cuts a block short at an access to a register that changes how
#     code is translated (the M4's CPACR write that turns the FPU on), and
#     the cut block is counted whole and then again from the cut. So
#     QEMU's count is high by a constant: the boot's. It is measured with
#     a main that only returns, and every program must show exactly that
#     difference, so one instruction more or less anywhere fails. On the
#     micro:bit the nRF51 UART's registers cut blocks too, so a program
#     that prints shifts the difference; there, only the output and the
#     status are compared.
#     A program QEMU cannot finish (240 KiB of arrays on a 64 KiB part)
#     is not applicable.
#  2. The exception model (tests/golden/embsim/exc.c): SVC, PendSV,
#     nested NVIC interrupts, PRIMASK and BASEPRI, a UsageFault handled
#     and one escalated, SysTick waking WFI, a thread on the PSP, and the
#     FPU's extended frame. It must print what QEMU prints, and what
#     tests/golden/embsim/exc-*.txt record, so it is checked without
#     QEMU too.
#  3. The ends of a run: semihosting's exit status, a lockup, a loop
#     nothing can interrupt, --until and --max-insns.
#  4. Instruction edges no exec program reaches (tests/golden/embsim/isa.c,
#     built by clang): flags, overflow, long multiply, VFP conversion and
#     fused multiply, and the exception frame's alignment.
set -u
echo "TEST-MARKER embsim"
. "$(dirname "$0")/../lib.sh"

QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
export EMBCC EMBLD EMBSIM
out=tests/golden/out/embsim
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

have_qemu=0
command -v "$QA" >/dev/null 2>&1 && have_qemu=1
plug=
if [ $have_qemu = 1 ]; then
    inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    p=$PWD/$out/icount.so
    { cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -undefined dynamic_lookup -o "$p" tools/bench/icount.c ||
      cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
         -o "$p" tools/bench/icount.c; } 2>/dev/null && plug=$p
    [ -n "$plug" ] || echo "note: QEMU's counting plugin does not build; counts are not compared"
fi

# ---- 3. the ends of a run (no QEMU needed) --------------------------------
e=$out/ends; mkdir -p "$e"
cat > "$e/ends.c" <<'EOF'
extern unsigned __data_load, __data_start, __data_end;
void reset(void);
__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)0x20010000u, (void *)reset };
static volatile unsigned blk[2];
/* MODE 4: each frame's call returns to the same `pop {r4, pc}` its caller
 * then executes -- a branch to itself that moves sp, not an idle loop.
 * Returns how many frames were entered (n + 1). */
__attribute__((naked)) static unsigned depth(unsigned n, unsigned zero)
{
    __asm__("push {r4, lr}\n"
            "adds r1, r1, #1\n"
            "subs r0, r0, #1\n"
            "bmi 2f\n"
            "bl depth\n"
            "1: pop {r4, pc}\n"
            "2: movs r0, r1\n"
            "pop {r4, pc}\n");
}
static void semi(unsigned op, const void *a)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(a) : "r0", "r1", "memory");
}
void reset(void)
{
    semi(4, "ready\n");
#if MODE == 0
    blk[0] = 0x20026u; blk[1] = 77;
    semi(0x20, (const void *)blk);
#elif MODE == 1
    __builtin_trap();
#elif MODE == 2
    for (;;)
        ;
#elif MODE == 4
    blk[0] = 0x20026u; blk[1] = depth(5, 0) + 40;
    semi(0x20, (const void *)blk);
#else
    for (volatile int i = 0;; i++)
        if (i == 1000)
            semi(4, "==MARK==\n");
#endif
}
EOF
for m in 0 1 2 3 4; do
    "$EMBCC" --target=thumbv7m-none-eabi -O1 -DMODE=$m -c "$e/ends.c" -o "$e/e$m.o" &&
    "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$e/e$m.o" -o "$e/e$m.elf" ||
        fail "the end-of-run images do not build"
done
"$EMBSIM" "$e/e0.elf" --max-insns 10000000 > "$e/0.out" 2>&1; st=$?
[ $st = 77 ] && grep -q ready "$e/0.out" || fail "SYS_EXIT_EXTENDED 77 gave status $st"
"$EMBSIM" "$e/e1.elf" --max-insns 10000000 > "$e/1.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'lockup' "$e/1.out" || { cat "$e/1.out"; fail "a fault with no handler is not a lockup (status $st)"; }
"$EMBSIM" "$e/e2.elf" --stats --max-insns 10000000 > "$e/2.out" 2>&1; st=$?
[ $st = 0 ] && grep -q 'nothing can interrupt' "$e/2.out" || { cat "$e/2.out"; fail "an idle loop does not end the run (status $st)"; }
"$EMBSIM" "$e/e4.elf" --max-insns 10000000 > "$e/5.out" 2>&1; st=$?
[ $st = 46 ] || { cat "$e/5.out"; fail "returns landing on the same pop were taken for an idle loop (status $st)"; }
"$EMBSIM" "$e/e3.elf" --until '==MARK==' --max-insns 10000000 > "$e/3.out" 2>&1; st=$?
[ $st = 0 ] && [ "$(grep -c MARK "$e/3.out")" = 1 ] || fail "--until does not stop at the first ==MARK== (status $st)"
"$EMBSIM" "$e/e3.elf" --max-insns 500 > "$e/4.out" 2>&1; st=$?
[ $st = 4 ] && grep -q '500 instructions' "$e/4.out" || { cat "$e/4.out"; fail "--max-insns 500: status $st"; }
echo "embsim: semihosting exit, lockup, idle loop (and not a pop returning to itself), --until and --max-insns end a run as documented"

x=$out/exc; mkdir -p "$x"
# ---- 4. instruction edges the corpus never reaches ---------------------------
# tests/golden/embsim/isa.c: asrs's carry, sdiv of INT_MIN by -1, umlal's
# accumulation, vcvt's truncation, vfms and vfma, and an exception taken
# with sp 4 below an 8-byte boundary. Built by clang, whose assembler has
# every one of these forms; EmbCC's inline assembler does not.
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 \
       -mfloat-abi=hard -O1 -ffreestanding -fno-builtin \
       -c tests/golden/embsim/isa.c -o "$x/isa.o" 2>/dev/null; then
    "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$x/isa.o" -o "$x/isa.elf" ||
        fail "isa.c does not link"
    "$EMBSIM" "$x/isa.elf" --board mps2-an386 --max-insns 10000000 > "$x/isa.sim" 2>&1 ||
        { cat "$x/isa.sim"; fail "isa.c: EmbSim does not exit 0"; }
    cmp -s "$x/isa.sim" tests/golden/embsim/isa.txt || {
        diff tests/golden/embsim/isa.txt "$x/isa.sim"
        fail "isa.c: EmbSim's output is not isa.txt"; }
    if [ $have_qemu = 1 ]; then
        # shellcheck disable=SC2086
        sh tests/harness/qrun.sh 20 "$QA" -M mps2-an386 -cpu cortex-m4 -nographic \
            -chardev file,id=semi,path="$x/isa.qemu" \
            -semihosting-config enable=on,chardev=semi \
            -kernel "$x/isa.elf" > /dev/null 2>&1
        cmp -s "$x/isa.qemu" "$x/isa.sim" || {
            diff "$x/isa.qemu" "$x/isa.sim"
            fail "isa.c: EmbSim's output is not QEMU's"; }
    fi
    echo "embsim: asrs's carry, sdiv's overflow, umlal, vcvt, vfms/vfma and an unaligned exception frame as QEMU and the record have them"
else
    echo "embsim: no clang for thumbv7em: the instruction-edge program was not built"
fi

# ---- the configurations -----------------------------------------------------
# TAG TRIPLE EXTRA-CFLAGS HARNESS HARNESS-CFLAGS OPT QEMU-ARGS EMBSIM-ARGS COUNTS
configs="m3|thumbv7m-none-eabi||thumb||-O0|-M lm3s6965evb -cpu cortex-m3|--board lm3s6965evb|1
m4|thumbv7em-none-eabi||thumb-m4f|-DSRAM_TOP=0x20400000u|-Os|-M mps2-an386 -cpu cortex-m4|--board mps2-an386|1
m4hf|thumbv7em-none-eabihf||thumb-m4f|-DSRAM_TOP=0x20400000u|-O2|-M mps2-an386 -cpu cortex-m4|--board mps2-an386|1
m7|thumbv7em-none-eabihf|-mcpu=cortex-m7|thumb-m4f|-DSRAM_TOP=0x20400000u|-O2|-M mps2-an500 -cpu cortex-m7|--board mps2-an500|1
m0|thumbv6m-none-eabi||thumb-m0|-DSRAM_TOP=0x20010000u|-O2|-M microbit -global nrf51-soc.sram-size=65536|--board microbit --ram-size 64K|0"

drv_src() {
    cat <<'EOT'
int prog_main(void);
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
static volatile unsigned blk[2];
int main(void)
{
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    blk[0] = 0x20026u;
    blk[1] = (unsigned)prog_main();
    __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab"
                     : : "r"(blk) : "r0", "r1", "memory");
    for (;;)
        ;
}
EOT
}

# one.sh TAG NAME: build one program and run it on both; one line of result
cat > "$out/one.sh" <<'EOT'
tag=$1; c=$2; out=$3
. "$out/$tag/cfg"
n=$(basename "$c" .c); o=$out/$tag/p/$n
if ! "$CC" --target=$T $X $OPT -Dmain=prog_main -Ilib/libc/include -c "$c" -o $o.o 2>/dev/null ||
   ! EMBCC_THUMB_HARNESS=$out/$tag EMBCC_THUMB_M0_HARNESS=$out/$tag \
       sh tests/harness/$H/link.sh $o.elf $o.o $out/$tag/drv.o \
       $out/$tag/libc.a $out/$tag/librt.a > /dev/null 2>&1; then
    echo "NA $tag $n"; exit 0
fi
# a budget, so a simulator that loops fails here instead of hanging the
# suite: 4G instructions, over three times the corpus's longest program
# (div-const on the M0, 1.19G: a divide by a constant is a helper call)
"$EMBSIM" $o.elf $EA --max-insns 4000000000 --count $o.en > $o.eout 2> $o.eerr; es=$?
if [ "$QA" = - ]; then
    echo "SIM $tag $n $es"; exit 0
fi
pl=; [ -n "$PLUG" ] && pl="-plugin $PLUG,out=$o.qn"
sh tests/harness/qrun.sh 30 "$QA" $QARGS -semihosting -nographic $pl \
    -kernel $o.elf > $o.qout 2> /dev/null; qs=$?
if [ $qs -ge 124 ]; then
    echo "NA $tag $n (QEMU did not finish)"; exit 0
fi
if [ $qs != $es ] || ! cmp -s $o.qout $o.eout; then
    echo "DIFF $tag $n: QEMU exits $qs, EmbSim $es$(cmp -s $o.qout $o.eout || echo ', and the output differs')$(head -c 200 $o.eerr | tr '\n' ' ')"
    exit 0
fi
if [ "$COUNTS" = 1 ] && [ -s $o.qn ]; then
    qi=$(sed -n 1p $o.qn); qc=$(sed -n 2p $o.qn)
    ei=$(sed -n 1p $o.en); ec=$(sed -n 2p $o.en)
    echo "COUNT $tag $n $((qi - ei)) $((qc - ec)) $ei"
else
    echo "SAME $tag $n"
fi
EOT

pids=
echo "$configs" | while IFS='|' read tag T X H HF OPT QARGS EA COUNTS; do
    d=$out/$tag; mkdir -p "$d/p"
    CC=$EMBCC
    if [ -n "$X" ]; then
        printf 'exec "%s" "$@" %s\n' "$EMBCC" "$X" > "$d/cc"; chmod +x "$d/cc"
        CC=$PWD/$d/cc
    fi
    machine=${QARGS#-M }; machine=${machine%% *}
    qa=$QA
    if [ $have_qemu = 0 ] || ! "$QA" -M help 2>/dev/null | grep -q "^$machine "; then
        qa=-
    fi
    {
        echo "T=$T; X='$X'; H=$H; OPT=$OPT; CC=$CC; COUNTS=$COUNTS"
        echo "QARGS='$QARGS'; EA='$EA'; QA=$qa; PLUG=$plug"
    } > "$d/cfg"
    { EMBCC=$CC sh tools/build-rt.sh $T "$d" && EMBCC=$CC sh tools/build-libc.sh $T "$d"; } \
        > "$d/build.log" 2>&1 || fail "$tag: lib/rt or lib/libc does not build"
    for f in boot io; do
        "$CC" --target=$T $HF -c tests/harness/$H/$f.c -o "$d/$f.o" ||
            fail "$tag: the harness does not compile"
    done
    drv_src > "$d/drv.c"
    "$CC" --target=$T -O1 -c "$d/drv.c" -o "$d/drv.o" || fail "$tag: the driver"
    # the boot's own difference in QEMU's count: a main that returns
    printf 'int main(void) { return 0; }\n' > "$d/empty.c"
    sh "$out/one.sh" $tag "$d/empty.c" "$out" > "$d/empty.result"
done || exit 1

for tag in m3 m4 m4hf m7 m0; do
    for c in tests/exec/*.c; do echo "$tag $c $out"; done
done | xargs -P "${EMBCC_JOBS:-8}" -n 3 sh "$out/one.sh" > "$out/results.txt" 2>&1

total=0
for tag in m3 m4 m4hf m7 m0; do
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
            fail "$tag: QEMU's count differs from EmbSim's by other than the boot's $bi instructions and $bc cycles"; }
    fi
    same=$(grep -c "^\(COUNT\|SAME\) $tag " "$r")
    sim=$(grep -c "^SIM $tag " "$r")
    na=$(grep -c "^NA $tag " "$r")
    insns=$(grep "^COUNT $tag " "$r" | awk '{ s += $6 } END { print s + 0 }')
    if [ $sim -gt 0 ]; then
        echo "embsim: $tag: $sim programs ran (no QEMU for this board to compare with); $na not applicable"
    elif [ -n "$bi" ]; then
        echo "embsim: $tag: $same programs as QEMU, to the instruction and the cycle ($insns instructions; the boot's offset $bi); $na not applicable"
    else
        echo "embsim: $tag: $same programs as QEMU, output and exit status; $na not applicable"
    fi
    total=$((total + same))
done
[ $have_qemu = 0 ] || [ $total -ge 800 ] || fail "only $total programs were compared"

# ---- 2. the exception model -------------------------------------------------
x=$out/exc; mkdir -p "$x"
for cfg in "m3|thumbv7m-none-eabi||--board lm3s6965evb|-M lm3s6965evb -cpu cortex-m3" \
           "m4hf|thumbv7em-none-eabihf||--board mps2-an386|-M mps2-an386 -cpu cortex-m4" \
           "m0|thumbv6m-none-eabi||--board microbit|-M microbit"; do
    IFS='|' read tag T X EA QARGS <<EOF
$cfg
EOF
    "$EMBCC" --target=$T -O2 -c tests/golden/embsim/exc.c -o "$x/$tag.o" &&
    "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$x/$tag.o" "$out/$tag/librt.a" \
        -o "$x/$tag.elf" ||
        fail "exc.c does not build for $tag"
    # shellcheck disable=SC2086
    "$EMBSIM" "$x/$tag.elf" $EA --max-insns 100000000 > "$x/$tag.sim" 2> "$x/$tag.err"; st=$?
    [ $st = 0 ] || { cat "$x/$tag.sim" "$x/$tag.err"; fail "exc.c on $tag: EmbSim exits $st"; }
    cmp -s "$x/$tag.sim" "tests/golden/embsim/exc-$tag.txt" || {
        diff "tests/golden/embsim/exc-$tag.txt" "$x/$tag.sim"
        fail "exc.c on $tag: EmbSim's output is not exc-$tag.txt"; }
    if [ $have_qemu = 1 ]; then
        # QEMU writes semihosting's console to stderr unless it is given
        # a character device of its own
        # shellcheck disable=SC2086
        sh tests/harness/qrun.sh 20 "$QA" $QARGS -nographic \
            -chardev file,id=semi,path="$x/$tag.qemu" \
            -semihosting-config enable=on,chardev=semi \
            -kernel "$x/$tag.elf" > /dev/null 2>&1
        cmp -s "$x/$tag.qemu" "$x/$tag.sim" || {
            diff "$x/$tag.qemu" "$x/$tag.sim"
            fail "exc.c on $tag: EmbSim's output is not QEMU's"; }
    fi
done
echo "embsim: exceptions (SVC, PendSV, nesting, masking, faults, SysTick, PSP, FP frame) as QEMU and the record have them, on m3, m4hf and m0"

