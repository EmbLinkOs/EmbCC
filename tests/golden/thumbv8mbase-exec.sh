#!/bin/sh
# What ARMv8-M Baseline code (thumbv8m.base-none-eabi, the Cortex-M23)
# COMPUTES, and that it is Baseline code.
#
# QEMU models no Cortex-M23. The images run on the Cortex-M33 of QEMU's
# mps2-an505 (tests/harness/thumb-m23), which executes every Baseline
# instruction -- Baseline is a subset of Mainline -- with unaligned accesses
# made to fault as they do on an M23. What the M33 cannot show is an
# instruction the M23 lacks, which is what part 3 is for.
#
#  1. The exec corpus. Every tests/exec/*.c program, compiled at -O0, -O1,
#     -O2 and -Os, linked with lib/libc and lib/rt built for the triple, and
#     run; the exit status must be its `// expect-exit: N`. The corpus was
#     written for hosted LP64 targets, so a program that gives the wrong
#     answer is RE-RUN as an ARMv8-M Mainline build on the same board: when
#     that is wrong too (a `long` assumed to be eight bytes, __int128, a
#     feature the Thumb backend refuses at both levels) the program is not
#     applicable here, and is counted as such rather than hidden. A program
#     Mainline runs right and Baseline does not fails this test.
#  2. The golden embedded programs: embedded-stress.c against clang's
#     thumbv8m.base build of it on the same board; embedded-int64.c,
#     -float.c, -aggregate.c, -varargs.c and -divatomic.c (the divides and
#     the atomics this level makes instructions of) against the host.
#  3. A scan of every object built -- the programs at every level, lib/libc,
#     lib/rt and the harness -- disassembled by llvm-objdump for
#     thumbv8m.base: any 32-bit encoding but the ones Baseline has, an IT,
#     or anything it cannot decode is an instruction a Cortex-M23 faults on;
#     two mapping symbols at one address make the data there read as code.
#     And the divides and the exclusives must be THERE: an sdiv, a udiv, an
#     ldrex and a strex in the corpus, and no call to __aeabi_idiv,
#     __aeabi_uidiv[mod] or __atomic_* from any of it.
set -u
echo "TEST-MARKER thumbv8mbase-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
"$QEMU" -M help 2>/dev/null | grep -q '^mps2-an505' || {
    echo "SKIP: this $QEMU has no mps2-an505 (Cortex-M33) machine"; exit 0; }

T=thumbv8m.base-none-eabi
M=thumbv8m.main-none-eabi
H=tests/harness/thumb-m23
out=tests/golden/out/thumbv8mbase-exec
rm -rf "$out"; mkdir -p "$out/lb" "$out/lm" "$out/p"
export EMBCC
EMBLD=${EMBLD:-$PWD/embld}
export EMBLD

# ---- the runtime, the library and the harness, for both levels -----------
sh tools/build-rt.sh $T "$out/lb" > "$out/lb/build.log" 2>&1 &&
sh tools/build-libc.sh $T "$out/lb" >> "$out/lb/build.log" 2>&1 || {
    echo "lib/rt or lib/libc does not build for $T:"
    tail -3 "$out/lb/build.log"; exit 1; }
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "$H/$f.c" -o "$out/lb/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
refm=1
{ sh tools/build-rt.sh $M "$out/lm" &&
  sh tools/build-libc.sh $M "$out/lm"; } > "$out/lm/build.log" 2>&1 || refm=0
for f in boot io; do
    "$EMBCC" --target=$M -O1 -c "$H/$f.c" -o "$out/lm/$f.o" || refm=0
done

# ---- 1. the corpus ---------------------------------------------------------
cat > "$out/one.sh" <<'EOF'
c=$1; opt=$2; out=$3; refm=$4; H=tests/harness/thumb-m23
T=thumbv8m.base-none-eabi; M=thumbv8m.main-none-eabi
name=$(basename "$c" .c)
expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
[ -n "$expect" ] || exit 0
o=$out/p/$name$opt
if ! "$EMBCC" --target=$T $opt -Ilib/libc/include -c "$c" -o $o.o 2> $o.err; then
    if "$EMBCC" --target=$M $opt -Ilib/libc/include -c "$c" \
         -o $o.m.o > /dev/null 2>&1; then
        echo "FAIL $name $opt: does not compile for Baseline: $(grep -m1 error $o.err)"
    else
        echo "NA $name $opt: Mainline refuses it too"
    fi
    exit 0
fi
if ! EMBCC_M23_HARNESS=$out/lb sh $H/link.sh $o.elf $o.o \
       $out/lb/libc.a $out/lb/librt.a > $o.lerr 2>&1; then
    echo "NA $name $opt: does not link here: $(head -1 $o.lerr)"
    exit 0
fi
sh $H/run.sh $o.elf > $o.txt 2>&1
got=$?
[ "$got" = "$expect" ] && { echo "PASS $name $opt"; exit 0; }
gm=none
if [ "$refm" = 1 ] &&
   "$EMBCC" --target=$M $opt -Ilib/libc/include -c "$c" -o $o.m.o \
       > /dev/null 2>&1 &&
   EMBCC_M23_HARNESS=$out/lm sh $H/link.sh $o.m.elf $o.m.o \
       $out/lm/libc.a $out/lm/librt.a > /dev/null 2>&1; then
    sh $H/run.sh $o.m.elf > $o.m.txt 2>&1
    gm=$?
fi
if [ "$gm" = "$expect" ]; then
    echo "FAIL $name $opt: exit $got, want $expect (Mainline: $gm) $(grep -m1 FAULT $o.txt)"
else
    echo "NA $name $opt: exit $got, and Mainline gives $gm too"
fi
EOF
for opt in -O0 -O1 -O2 -Os; do
    for c in tests/exec/*.c; do
        echo "$c $opt $out $refm"
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
    EMBCC_M23_HARNESS=$out/lb sh $H/link.sh "$out/$tag.elf" "$@" \
        "$out/lb/libc.a" "$out/lb/librt.a" || {
        echo "$tag: does not link"; return 1; }
    sh $H/run.sh "$out/$tag.elf" > "$out/$tag.raw" 2>&1
    sed -n '1,/==END==/p' "$out/$tag.raw" > "$out/$tag.txt"
    grep -q '==END==' "$out/$tag.txt" || {
        echo "$tag: did not reach the end of main:"; head -5 "$out/$tag.raw"
        return 1; }
}
if command -v "$CLANG" >/dev/null 2>&1; then
    "$CLANG" --target=$T -mcpu=cortex-m23 -ffreestanding -Os \
        -c tests/golden/embedded-stress.c -o "$out/stress-ref.o" &&
        gold stress-ref "$out/stress-ref.o" || fail=1
fi
for p in int64 float aggregate varargs divatomic; do
    cc -std=c99 -w -ffp-contract=off -o "$out/host-$p" \
       tests/golden/embedded-$p.c tests/harness/thumb/hostio.c -lm &&
    "$out/host-$p" > "$out/$p-ref.txt" || {
        echo "embedded-$p.c: the host reference failed"; fail=1; }
done
for opt in -O0 -O1 -O2 -Os; do
    for p in stress int64 float aggregate varargs divatomic; do
        "$EMBCC" --target=$T $opt -Ilib/libc/include \
            -c tests/golden/embedded-$p.c -o "$out/$p$opt.o" || {
            echo "embedded-$p.c $opt: does not compile"; fail=1; continue; }
        gold "$p$opt" "$out/$p$opt.o" || { fail=1; continue; }
        if [ "$p" = stress ]; then
            [ -f "$out/stress-ref.txt" ] || continue
            ref="$out/stress-ref.txt"
        else
            ref="$out/$p-ref.txt"
        fi
        diff -u "$ref" "$out/$p$opt.txt" > "$out/$p$opt.diff" || {
            echo "embedded-$p.c at $opt does not agree with its reference:"
            head -12 "$out/$p$opt.diff"; fail=1; }
    done
done
[ "$fail" = 0 ] &&
echo "embedded-stress (against clang), -int64, -float, -aggregate, -varargs and -divatomic (against the host) agree at four levels"

# ---- 3. every instruction is ARMv8-M Baseline ------------------------------
if command -v "$OD" >/dev/null 2>&1; then
    find "$out" -name '*.o' ! -name '*.m.o' ! -path "$out/lm/*" \
         ! -name 'stress-ref.o' > "$out/objs.txt"
    : > "$out/scan.txt"; : > "$out/count.txt"; : > "$out/mn.txt"
    while read -r o; do
        "$OD" -dr --triple=thumbv8m.base "$o" 2>/dev/null |
        awk -v obj="$o" -v mn="$out/mn.txt" '
            /R_ARM_THM_(CALL|JUMP24)/ {
                if ($NF ~ /^(__aeabi_u?idiv(mod)?|__atomic_|__sync_)/)
                    print obj ": a call to " $NF " on a core that has the instruction"
                next
            }
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
                print m >> mn
                if (rest ~ /<unknown>/) { print obj ": " $0; next }
                if (m ~ /^it/) { print obj ": " $0; next }
                if (enc ~ /^[0-9a-f][0-9a-f][0-9a-f][0-9a-f] [0-9a-f][0-9a-f][0-9a-f][0-9a-f]$/ &&
                    m !~ /^(bl|b\.w|mrs|msr|dmb|dsb|isb|sdiv|udiv|movw|movt|clrex|sg|ldrexb?h?|strexb?h?|ldaex?[bh]?|stlex?[bh]?|ttt?a?t?)$/)
                    print obj ": " $0
            }
            END { print "COUNT " n > "/dev/stderr" }' >> "$out/scan.txt" \
            2>> "$out/count.txt"
        "$OD" -t "$o" 2>/dev/null |
        awk -v obj="$o" '$NF ~ /^\$[td]$/ { k = $3 ":" $1
            if (n[k]++) print obj ": two mapping symbols at " k }' \
            >> "$out/scan.txt"
    done < "$out/objs.txt"
    if [ -s "$out/scan.txt" ]; then
        echo "an instruction a Cortex-M23 does not have, a helper call where it"
        echo "has the instruction, or data read as code:"
        head -10 "$out/scan.txt"
        fail=1
    else
        echo "$(wc -l < "$out/objs.txt" | tr -d ' ') objects, $(awk '{s += $2} END {print s}' "$out/count.txt") instructions: all ARMv8-M Baseline"
    fi
    for ins in sdiv udiv ldrex strex ldrexb strexb ldrexh strexh; do
        k=$(grep -cx "$ins" "$out/mn.txt")
        [ "$k" -gt 0 ] || { echo "no $ins anywhere in the corpus"; fail=1; }
        printf '%s %s  ' "$ins" "$k"
    done
    echo
else
    echo "SKIP the scan: no llvm-objdump"
fi
exit $fail
