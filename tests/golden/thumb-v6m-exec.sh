#!/bin/sh
# What the ARMv6-M backend (src/arch/thumb/v6m.c) COMPUTES, on a Cortex-M0:
# QEMU's micro:bit, through tests/harness/thumb-m0.
#
#  1. The exec corpus. Every tests/exec/*.c program, compiled at -O0, -O1,
#     -O2 and -Os, linked with lib/libc and lib/rt built for the triple, and
#     run; the exit status must be its `// expect-exit: N`. The corpus was
#     written for hosted LP64 targets, so a program that gives the wrong
#     answer is RE-RUN as an ARMv7-M build on the Cortex-M3 board: when that
#     is wrong too (a `long` assumed to be eight bytes, __int128, a feature
#     the Thumb backend refuses at both levels) the program is not
#     applicable here, and is counted as such rather than hidden. A program
#     ARMv7-M runs right and ARMv6-M does not fails this test.
#  2. The golden embedded programs, as thumb-exec.sh runs them on the M3:
#     embedded-stress.c against clang's thumbv6m build of it on the same
#     board, embedded-int64.c and embedded-float.c against the host.
#  3. A scan of every object built -- the programs at every level, lib/libc,
#     lib/rt and the harness -- disassembled by llvm-objdump for thumbv6m:
#     any 32-bit encoding but BL, MRS, MSR, DMB, DSB and ISB, or anything it
#     cannot decode, is an instruction a Cortex-M0 faults on.
#
# The SoC's SRAM is raised to 64 KiB for the corpus, as the Cortex-M3 board
# has (the micro:bit's own 16 KiB would turn a large test's stack into a
# hang); the core is the same Cortex-M0, which is what is under test.
set -u
echo "TEST-MARKER thumb-v6m-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
"$QEMU" -M help 2>/dev/null | grep -q '^microbit' || {
    echo "SKIP: this $QEMU has no micro:bit (Cortex-M0) machine"; exit 0; }

T=thumbv6m-none-eabi
H=tests/harness/thumb-m0
out=tests/golden/out/thumb-v6m-exec
rm -rf "$out"; mkdir -p "$out/l6" "$out/l7" "$out/p"
export EMBCC
EMBCC_ABS=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLD=${EMBLD:-$PWD/embld}
export EMBLD

# ---- the runtime, the library and the harness, for both boards ----------
sh tools/build-rt.sh $T "$out/l6" > "$out/l6/build.log" 2>&1 &&
sh tools/build-libc.sh $T "$out/l6" >> "$out/l6/build.log" 2>&1 || {
    echo "lib/rt or lib/libc does not build for $T:"
    tail -3 "$out/l6/build.log"; exit 1; }
for f in boot io; do
    "$EMBCC" --target=$T -O1 -DSRAM_TOP=0x20010000u -c "$H/$f.c" \
        -o "$out/l6/$f.o" || { echo "the harness does not compile"; exit 1; }
done
ref7=1
{ sh tools/build-rt.sh thumbv7m-none-eabi "$out/l7" &&
  sh tools/build-libc.sh thumbv7m-none-eabi "$out/l7"; } \
    > "$out/l7/build.log" 2>&1 || ref7=0
for f in boot io; do
    "$EMBCC" --target=thumbv7m-none-eabi -O1 -DHARNESS_LM3S \
        -DSRAM_TOP=0x20010000u -c "$H/$f.c" -o "$out/l7/$f.o" || ref7=0
done

# ---- 1. the corpus ---------------------------------------------------------
# One program at one level per call, so they run side by side; each writes
# one line: PASS, FAIL, or NA with the reason. A run stops at its exit line,
# so the time limit costs only a program that hangs; it is generous because
# ARMv6-M divides in software -- div-const at -O0 needs 4 s of QEMU on a
# loaded machine, and at 8 s it timed out with eight runs side by side.
cat > "$out/one.sh" <<'EOF'
c=$1; opt=$2; out=$3; ref7=$4; H=tests/harness/thumb-m0
name=$(basename "$c" .c)
expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
[ -n "$expect" ] || exit 0
o=$out/p/$name$opt
if ! "$EMBCC" --target=thumbv6m-none-eabi $opt -Ilib/libc/include -c "$c" \
       -o $o.o 2> $o.err; then
    if "$EMBCC" --target=thumbv7m-none-eabi $opt -Ilib/libc/include -c "$c" \
         -o $o.v7.o > /dev/null 2>&1; then
        echo "FAIL $name $opt: does not compile for ARMv6-M: $(grep -m1 error $o.err)"
    else
        echo "NA $name $opt: ARMv7-M refuses it too"
    fi
    exit 0
fi
if ! EMBCC_THUMB_M0_HARNESS=$out/l6 sh $H/link.sh $o.elf $o.o \
       $out/l6/libc.a $out/l6/librt.a > $o.lerr 2>&1; then
    echo "NA $name $opt: does not link here: $(head -1 $o.lerr)"
    exit 0
fi
EMBCC_M0_SRAM=65536 sh $H/run.sh $o.elf > $o.txt 2>&1
got=$?
[ "$got" = "$expect" ] && { echo "PASS $name $opt"; exit 0; }
g7=none
if [ "$ref7" = 1 ] &&
   "$EMBCC" --target=thumbv7m-none-eabi $opt -Ilib/libc/include -c "$c" \
       -o $o.v7.o > /dev/null 2>&1 &&
   EMBCC_THUMB_M0_HARNESS=$out/l7 sh $H/link.sh $o.v7.elf $o.v7.o \
       $out/l7/libc.a $out/l7/librt.a > /dev/null 2>&1; then
    sh $H/run-m3.sh $o.v7.elf > $o.v7.txt 2>&1
    g7=$?
fi
if [ "$g7" = "$expect" ]; then
    echo "FAIL $name $opt: exit $got, want $expect (ARMv7-M on the M3: $g7) $(grep -m1 FAULT $o.txt)"
else
    echo "NA $name $opt: exit $got, and ARMv7-M on the M3 gives $g7 too"
fi
EOF
for opt in -O0 -O1 -O2 -Os; do
    for c in tests/exec/*.c; do
        echo "$c $opt $out $ref7"
    done
done | EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
       xargs -P "${EMBCC_JOBS:-8}" -n 4 sh "$out/one.sh" > "$out/results.txt" 2>&1
fail=0
for opt in -O0 -O1 -O2 -Os; do
    p=$(grep -c "^PASS .* $opt\$" "$out/results.txt")
    n=$(grep -c "^NA .* $opt:" "$out/results.txt")
    f=$(grep -c "^FAIL .* $opt:" "$out/results.txt")
    echo "exec corpus $opt: $p pass, $f fail, $n not applicable"
    [ "$f" = 0 ] || fail=1
done
grep '^FAIL' "$out/results.txt" | head -20
[ "$(grep -c '^PASS' "$out/results.txt")" -ge 600 ] || {
    echo "fewer than 600 corpus runs passed -- the harness, not the programs"
    fail=1; }

# ---- 2. the golden programs ------------------------------------------------
gold() {             # gold TAG OBJ... -> $out/TAG.txt, up to ==END==
    tag=$1; shift
    EMBCC_THUMB_M0_HARNESS=$out/l6 sh $H/link.sh "$out/$tag.elf" "$@" \
        "$out/l6/libc.a" "$out/l6/librt.a" || {
        echo "$tag: does not link"; return 1; }
    EMBCC_M0_SRAM=65536 sh $H/run.sh "$out/$tag.elf" > "$out/$tag.raw" 2>&1
    sed -n '1,/==END==/p' "$out/$tag.raw" > "$out/$tag.txt"
    grep -q '==END==' "$out/$tag.txt" || {
        echo "$tag: did not reach the end of main:"; head -5 "$out/$tag.raw"
        return 1; }
}
if command -v "$CLANG" >/dev/null 2>&1; then
    "$CLANG" --target=$T -ffreestanding -Os -c tests/golden/embedded-stress.c \
        -o "$out/stress-ref.o" && gold stress-ref "$out/stress-ref.o" || fail=1
fi
cc -std=c99 -w -o "$out/host64" tests/golden/embedded-int64.c \
   tests/harness/thumb/hostio.c && "$out/host64" > "$out/int64-ref.txt" || fail=1
cc -std=c99 -w -o "$out/hostfp" tests/golden/embedded-float.c \
   tests/harness/thumb/hostio.c -lm && "$out/hostfp" > "$out/float-ref.txt" ||
   fail=1
for opt in -O0 -O1 -O2 -Os; do
    for p in stress int64 float; do
        "$EMBCC" --target=$T $opt -Ilib/libc/include \
            -c tests/golden/embedded-$p.c -o "$out/$p$opt.o" || {
            echo "embedded-$p.c $opt: does not compile"; fail=1; continue; }
        gold "$p$opt" "$out/$p$opt.o" || { fail=1; continue; }
        case $p in
            stress) [ -f "$out/stress-ref.txt" ] || continue
                    ref="$out/stress-ref.txt" ;;
            int64)  ref="$out/int64-ref.txt" ;;
            float)  ref="$out/float-ref.txt" ;;
        esac
        diff -u "$ref" "$out/$p$opt.txt" > "$out/$p$opt.diff" || {
            echo "embedded-$p.c at $opt does not agree with its reference:"
            head -12 "$out/$p$opt.diff"; fail=1; }
    done
done
[ "$fail" = 0 ] &&
echo "embedded-stress (against clang), -int64 and -float (against the host) agree at four levels"

# ---- 3. every instruction is ARMv6-M ------------------------------------------
if command -v "$OD" >/dev/null 2>&1; then
    find "$out" -name '*.o' ! -name '*.v7.o' ! -path "$out/l7/*" \
         ! -name 'stress-ref.o' > "$out/objs.txt"
    : > "$out/scan.txt"
    while read -r o; do
        "$OD" -d --triple=thumbv6m "$o" 2>/dev/null |
        awk -v obj="$o" '
            # "     642: 6813         \tldr\tr3, [r2]": the encoding, a tab,
            # the mnemonic. Data (a pool, a table) is .word/.short/.byte.
            /^ *[0-9a-f]+: / {
                line = $0
                sub(/^ *[0-9a-f]+: /, "", line)
                t = index(line, "\t")
                if (!t) next
                enc = substr(line, 1, t - 1); gsub(/ +$/, "", enc)
                rest = substr(line, t + 1)
                m = rest; sub(/\t.*/, "", m)
                if (m ~ /^\./) next
                n++
                if (rest ~ /<unknown>/) { print obj ": " $0; next }
                if (m ~ /^it/ || m == "cbz" || m == "cbnz") {
                    print obj ": " $0; next }
                if (enc ~ /^[0-9a-f][0-9a-f][0-9a-f][0-9a-f] [0-9a-f][0-9a-f][0-9a-f][0-9a-f]$/ &&
                    m != "bl" && m != "mrs" && m != "msr" &&
                    m != "dmb" && m != "dsb" && m != "isb")
                    print obj ": " $0
            }
            END { print "COUNT " n > "/dev/stderr" }' >> "$out/scan.txt" \
            2>> "$out/count.txt"
    done < "$out/objs.txt"
    if [ -s "$out/scan.txt" ]; then
        echo "an instruction a Cortex-M0 does not have:"
        head -10 "$out/scan.txt"
        fail=1
    else
        echo "$(wc -l < "$out/objs.txt" | tr -d ' ') objects, $(awk '{s += $2} END {print s}' "$out/count.txt") instructions: all ARMv6-M"
    fi
else
    echo "SKIP the scan: no llvm-objdump"
fi
exit $fail
