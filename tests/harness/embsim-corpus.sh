#!/bin/sh
# The exec corpus on an EmbSim board and on QEMU's model of a board with
# the same core: what the board tests (embsim-pico.sh, embsim-bluepill.sh,
# embsim-nrf52840dk.sh) hold the core to. Sourced, with these set:
#
#   CORP      the directory to work in (made here)
#   CTRIPLE   the target triple; CFLAGS_C, the extra flags (-mcpu=...)
#   CBOOT     the startup (tests/golden/embsim-pico/corpus-boot.c for
#             Cortex-M, tests/golden/embsim-esp32c3/corpus-boot.c for
#             RISC-V): output and the exit status by semihosting on both
#             machines, the exit as _exit(status)
#   CEXTRA    objects the EmbSim image needs first (the Pico's boot2), or ""
#   QLD, ELD  the linker scripts for QEMU's image and EmbSim's (the same
#             file when the board's memory is QEMU's)
#   QARGS     QEMU's machine arguments; QA the qemu-system binary
#   EARGS     EmbSim's (--board NAME), with EMBSIM_SVD_PATH exported
#   CMIN      the fewest programs that must be compared
#
# Every tests/exec program is built once with lib/libc and lib/rt and
# linked twice; the two runs must give the same output and exit status,
# and EmbSim's instruction and cycle counts must differ from QEMU's (its
# plugin, tools/bench/icount.c) by one constant, the boots': what a main
# that only returns shows. The result is CORP/summary; corpus_run returns
# 1 with the reason on stdout when they disagree, 2 when QEMU or its
# plugin is not there.
corpus_run() {
    command -v "$QA" > /dev/null 2>&1 || { echo "no $QA"; return 2; }
    mkdir -p "$CORP/p"
    why=$(qemu_plugin tools/bench/icount.c "$CORP/icount.so") ||
        { echo "QEMU's counting plugin does not build: $why"; return 2; }
    printf 'exec "%s" "$@" %s\n' "$EMBCC" "$CFLAGS_C" > "$CORP/cc"
    chmod +x "$CORP/cc"
    { EMBCC=$PWD/$CORP/cc sh tools/build-rt.sh "$CTRIPLE" "$CORP" &&
      EMBCC=$PWD/$CORP/cc sh tools/build-libc.sh "$CTRIPLE" "$CORP"; } \
        > "$CORP/build.log" 2>&1 || { echo "lib/rt or lib/libc does not build"; return 1; }
    cat > "$CORP/drv.c" <<'EOT'
int prog_main(void);
void _exit(int status) __attribute__((noreturn));
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
int main(void)
{
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    _exit(prog_main());
}
EOT
    "$CORP/cc" --target="$CTRIPLE" -O2 -c "$CBOOT" -o "$CORP/boot.o" &&
    "$CORP/cc" --target="$CTRIPLE" -O1 -c "$CORP/drv.c" -o "$CORP/drv.o" ||
        { echo "the corpus's startup does not build"; return 1; }
    printf 'int main(void) { return 0; }\n' > "$CORP/empty.c"
    cat > "$CORP/one.sh" <<'EOT'
# one.sh PROG.c: one program on QEMU and on EmbSim
f=$1; n=$(basename "$f" .c); o=$CORP/p/$n
"$CORP/cc" --target=$CTRIPLE -O2 -Dmain=prog_main -Ilib/libc/include -c "$f" -o $o.o 2>/dev/null &&
"$EMBLD" -T $QLD $CORP/boot.o $CORP/drv.o $o.o $CORP/libc.a $CORP/librt.a -o $o.q.elf 2>/dev/null &&
"$EMBLD" -T $ELD $CEXTRA $CORP/boot.o $CORP/drv.o $o.o $CORP/libc.a $CORP/librt.a -o $o.e.elf 2>/dev/null ||
    { echo "NA $n"; exit 0; }
"$EMBSIM" $o.e.elf $EARGS --max-insns 4000000000 --count $o.en > $o.eout 2> $o.eerr; es=$?
sh tests/harness/qrun.sh 30 "$QA" $QARGS \
    -chardev stdio,id=sh0 -semihosting-config enable=on,target=native,chardev=sh0 \
    -serial none -monitor none -nographic -plugin $CORP/icount.so,out=$o.qn \
    -kernel $o.q.elf > $o.qout 2> /dev/null; qs=$?
[ $qs -ge 124 ] && { echo "NA $n (QEMU did not finish)"; exit 0; }
if [ $qs != $es ] || ! cmp -s $o.qout $o.eout; then
    echo "DIFF $n: QEMU exits $qs, EmbSim $es$(cmp -s $o.qout $o.eout || echo ', and the output differs')"
    exit 0
fi
[ -s $o.qn ] || { echo "NA $n (no count from QEMU)"; exit 0; }
echo "COUNT $n $(($(sed -n 1p $o.en) - $(sed -n 1p $o.qn))) $(($(sed -n 2p $o.en) - $(sed -n 2p $o.qn))) $(sed -n 1p $o.en)"
EOT
    export CORP CTRIPLE CEXTRA QLD ELD QARGS QA EARGS EMBLD EMBSIM
    base=$(sh "$CORP/one.sh" "$CORP/empty.c")
    case $base in
        COUNT*) set -- $base; cbi=$3; cbc=$4 ;;
        *) echo "a main that returns does not run on both: $base"; return 1 ;;
    esac
    ls tests/exec/*.c | xargs -P "${EMBCC_JOBS:-8}" -n 1 sh "$CORP/one.sh" > "$CORP/results.txt" 2>&1
    if grep '^DIFF' "$CORP/results.txt"; then
        echo "EmbSim and QEMU disagree"
        return 1
    fi
    bad=$(grep '^COUNT' "$CORP/results.txt" | awk -v i="$cbi" -v k="$cbc" '$3 != i || $4 != k')
    if [ -n "$bad" ]; then
        echo "$bad" | head -5
        echo "EmbSim's count differs from QEMU's by other than the boots' $cbi instructions and $cbc cycles"
        return 1
    fi
    csame=$(grep -c '^COUNT' "$CORP/results.txt")
    cna=$(grep -c '^NA' "$CORP/results.txt")
    cinsns=$(grep '^COUNT' "$CORP/results.txt" | awk '{ s += $5 } END { print s + 0 }')
    [ "$csame" -ge "$CMIN" ] || { echo "only $csame programs were compared ($cna not applicable)"; return 1; }
    echo "$csame programs as QEMU's, to the instruction and the cycle ($cinsns instructions; the boots' $cbi apart); $cna not applicable" > "$CORP/summary"
    return 0
}
