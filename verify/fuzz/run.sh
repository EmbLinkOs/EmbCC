#!/bin/sh
# verify/fuzz/run.sh -- differential fuzzing of EmbCC against the host's
# compiler, on QEMU's boards.
#
#   sh verify/fuzz/run.sh [-g 2|3] [-b BOARDS] [-O "OPTS"] FIRST LAST
#   sh verify/fuzz/run.sh --selftest
#
# For each seed from FIRST to LAST, genN.py writes a random C program with
# no undefined behaviour. The host's compiler (clang, or $FUZZ_CC) builds
# it at -O0 with -ffp-contract=off, and its run gives the checksum. EmbCC
# then compiles the program with that checksum built in, at each of OPTS,
# for each board, and the program must return 42 there: it returns
# something else when its own checksum differs. A program whose host runs
# disagree under two stack fillings reads uninitialised memory: that is
# the generator's bug, reported and skipped.
#
#   -g 2   (default) integers, floating point, structs, unions, bitfields,
#          pointers, function-pointer tables; every board
#   -g 3   gen 2 plus __int128 operands; the 64-bit boards only
#   -b     a comma-separated subset of: x86 a64 m4 m0 rv32 rv64
#          (default: x86,a64,m4,rv32 for gen 2; x86,a64,rv64 for gen 3);
#          m0 is ARMv6-M (src/arch/thumb/v6m.c) on QEMU's micro:bit, its
#          SRAM raised to 64 KiB as thumb-v6m-exec.sh runs it
#   -O     the levels (default "-O0 -O2")
#
# Run from the tree root, after `make embcc embld rt-embedded`. EMBCC and
# EMBLD name the binaries (default ./embcc, ./embld). Every program that
# fails is kept in $FUZZ_OUT (default verify/fuzz/out) as eSEED.c, ready
# for reduce.py. A line per failure, then `seed N done`; EMBCC_VERIFY=1 is
# set, as the test suite sets it.
#
# --selftest checks the runner itself: one program, run once with its
# true checksum (it must pass) and once with a wrong one (it must fail on
# every board). A runner that cannot see a mismatch finds nothing.
set -u
gen=2; boards=; opts="-O0 -O2"; selftest=0
while [ $# -gt 0 ]; do
    case $1 in
    -g) gen=$2; shift 2 ;;
    -b) boards=$(echo "$2" | tr ',' ' '); shift 2 ;;
    -O) opts=$2; shift 2 ;;
    --selftest) selftest=1; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "run.sh: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
done
if [ $selftest = 0 ] && [ $# -ne 2 ]; then
    echo "usage: run.sh [-g 2|3] [-b BOARDS] [-O \"OPTS\"] FIRST LAST | --selftest" >&2
    exit 2
fi
case $gen in 2|3) ;; *) echo "run.sh: -g is 2 or 3 (gen 4 is run4.sh)" >&2; exit 2 ;; esac
[ -n "$boards" ] || { [ $gen = 3 ] && boards="x86 a64 rv64" || boards="x86 a64 m4 rv32"; }

ROOT=$(pwd)
[ -f "$ROOT/verify/fuzz/gen2.py" ] || { echo "run.sh: run it from the tree root" >&2; exit 2; }
F=$ROOT/verify/fuzz
EMBCC=${EMBCC:-$ROOT/embcc}; EMBLD=${EMBLD:-$ROOT/embld}
case $EMBCC in /*) ;; *) EMBCC=$ROOT/$EMBCC ;; esac
case $EMBLD in /*) ;; *) EMBLD=$ROOT/$EMBLD ;; esac
export EMBLD EMBCC_VERIFY=1
CC=${FUZZ_CC:-clang}
OUT=${FUZZ_OUT:-$F/out}
mkdir -p "$OUT"
W=$(mktemp -d "${TMPDIR:-/tmp}/embfuzz.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# the host reference: checksum of program $1, or nothing
host_ck() {
    "$CC" -O0 -w -ffp-contract=off $2 -Dmain=prog_main -c "$1" -o "$W/h.o" 2>/dev/null &&
    "$CC" "$W/h.o" "$F/hostdrv.c" -o "$W/h" 2>/dev/null && "$W/h"
}

# one board, one level: prints "exit N", or why there is none
board() {   # board NAME OPT FILE
    b=$1; o=$2; src=$3; d=$W/$b; mkdir -p "$d"
    case $b in
    x86)
        "$EMBCC" --target=x86_64-elf $o -c "$src" -o "$d/p.o" || { echo "compile failed"; return; }
        tests/harness/x86_64/link.sh -o "$d/p" "$d/p.o" > /dev/null 2>&1 || { echo "link failed"; return; }
        tests/harness/x86_64/run.sh "$d/p" > /dev/null 2>&1; echo "exit $?" ;;
    a64)
        "$EMBCC" --target=aarch64-elf $o -c "$src" -o "$d/p.o" || { echo "compile failed"; return; }
        EMBCC_HARNESS_WORK=$d tests/harness/aarch64/link.sh -o "$d/p.elf" "$d/p.o" > /dev/null 2>&1 ||
            { echo "link failed"; return; }
        tests/harness/aarch64/run.sh "$d/p.elf" > /dev/null 2>&1; echo "exit $?" ;;
    m4)
        T=thumbv7em-none-eabi
        [ -f "$d/boot.o" ] || {
            for f in boot io; do
                "$EMBCC" --target=$T -c tests/harness/thumb-m4f/$f.c -o "$d/$f.o" || return
            done
            "$EMBCC" --target=$T -O1 -c "$F/m4drv.c" -o "$d/drv.o" || return; }
        "$EMBCC" --target=$T $o -Dmain=sw_main -Ilib/libc/include -c "$src" -o "$d/p.o" ||
            { echo "compile failed"; return; }
        EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb-m4f/link.sh "$d/p.elf" "$d/p.o" \
            "$d/drv.o" build/libc/$T/librt.a > /dev/null 2>&1 || { echo "link failed"; return; }
        sh tests/harness/qrun.sh 60 qemu-system-arm -M mps2-an386 -cpu cortex-m4 \
            -semihosting -nographic -kernel "$d/p.elf" > /dev/null 2>&1; echo "exit $?" ;;
    m0)
        T=thumbv6m-none-eabi
        [ -f "$d/boot.o" ] || {
            for f in boot io; do
                "$EMBCC" --target=$T -O1 -DSRAM_TOP=0x20010000u \
                    -c tests/harness/thumb-m0/$f.c -o "$d/$f.o" || return
            done; }
        "$EMBCC" --target=$T $o -Ilib/libc/include -c "$src" -o "$d/p.o" ||
            { echo "compile failed"; return; }
        EMBCC_THUMB_M0_HARNESS=$d sh tests/harness/thumb-m0/link.sh "$d/p.elf" \
            "$d/p.o" build/libc/$T/librt.a > /dev/null 2>&1 || { echo "link failed"; return; }
        EMBCC_M0_SRAM=65536 EMBCC_QEMU_TIMEOUT=60 \
            sh tests/harness/thumb-m0/run.sh "$d/p.elf" > /dev/null 2>&1; echo "exit $?" ;;
    rv32|rv64)
        X=${b#rv}; T=riscv$X-unknown-elf
        [ -f "$d/boot.o" ] || {
            "$EMBCC" --target=$T -c "$F/rvboot.c" -o "$d/boot.o" &&
            "$EMBCC" --target=$T -c tests/harness/riscv/io.c -o "$d/io.o" || return; }
        "$EMBCC" --target=$T $o -Ilib/libc/include -c "$src" -o "$d/p.o" ||
            { echo "compile failed"; return; }
        EMBCC_RISCV_HARNESS=$d sh tests/harness/riscv/link.sh "$d/p.elf" "$d/p.o" \
            build/libc/$T/librt.a > /dev/null 2>&1 || { echo "link failed"; return; }
        sh tests/harness/qrun.sh 60 qemu-system-riscv$X -M virt -bios none -nographic \
            -m 8 -kernel "$d/p.elf" > /dev/null 2>&1; echo "exit $?" ;;
    *) echo "unknown board $b" ;;
    esac
}

if [ $selftest = 1 ]; then
    python3 "$F/gen$gen.py" 1 > "$W/p.c"
    ck=$(host_ck "$W/p.c" "") || { echo "selftest: the host build failed"; exit 1; }
    python3 "$F/gen$gen.py" 1 "$ck" > "$W/good.c"
    python3 "$F/gen$gen.py" 1 $((ck + 1)) > "$W/bad.c"
    bad=0
    for b in $boards; do
        r=$(board $b -O0 "$W/good.c" 2>&1 | tail -1)
        [ "$r" = "exit 42" ] || { echo "selftest $b: the true checksum gave '$r'"; bad=1; }
        r=$(board $b -O0 "$W/bad.c" 2>&1 | tail -1)
        [ "$r" != "exit 42" ] || { echo "selftest $b: a wrong checksum passed"; bad=1; }
    done
    [ $bad = 0 ] && echo "selftest: the runner tells a right checksum from a wrong one on $boards"
    exit $bad
fi

for seed in $(seq "$1" "$2"); do
    python3 "$F/gen$gen.py" "$seed" > "$W/p.c"
    ck=$(host_ck "$W/p.c" "") || { echo "seed $seed: host build failed"; continue; }
    ckp=$(host_ck "$W/p.c" "-ftrivial-auto-var-init=pattern")
    [ "$ckp" = "$ck" ] || { echo "seed $seed: GENERATOR reads uninitialised memory"; continue; }
    python3 "$F/gen$gen.py" "$seed" "$ck" > "$W/e.c"
    for o in $opts; do
        for b in $boards; do
            r=$(board $b $o "$W/e.c" 2>&1 | tail -1)
            if [ "$r" != "exit 42" ]; then
                echo "seed $seed $b $o: $r"
                cp "$W/e.c" "$OUT/e$gen-$seed.c"
            fi
        done
    done
    echo "seed $seed done"
done
