#!/bin/sh
# embsim --profile: instructions and cycles per function, self and
# inclusive, along the call paths the core executed.
#
# tests/golden/embsim-an/prof.c makes calls of known number along known
# paths -- a loop's calls, a function calling another twice, recursion
# five deep, a call through a pointer, a timer interrupt -- and is built
# at -O1 on the Cortex-M3, RV32 (with C, whose returns are C.JR, and
# without, whose are JALR) and the AVR, in their harnesses. For each:
#   - the counts add up exactly to --stats': the (total) row, the sum of
#     the rows, and the collapsed stacks, by cycles and by instructions;
#   - each function's self instructions are what an independent count
#     makes them: the --trace of the same run, each address given to the
#     function llvm-nm says holds it (on the AVR, the vector table's jump
#     to the handler is the handler's, as the profile counts it);
#   - the calls: a 3, b 1, leaf 6, rec 5, and the interrupt handler as
#     many as the ticks the program counted;
#   - the paths: main;a;leaf, main;b;leaf, main;leaf (the pointer),
#     main;rec x5, and the handler only on top of main or spin;
#   - each function's inclusive count is the sum of the paths it is on;
#   - the run is the same with and without --profile.
# And tests/golden/embsim/exc.c on the M3: SVC, PendSV, nested
# interrupts and faults, whose handlers' entries must be the program's
# own counts, and nested where it nests them.
set -u
echo "TEST-MARKER embsim-profile"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
NM=${EMBCC_LLVM_NM:-llvm-nm}
export EMBLD
out=tests/golden/out/embsim-profile
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/embsim-an/prof.c
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }
have_nm=0
command -v "$NM" >/dev/null 2>&1 && have_nm=1

build() {
    tag=$1; h=$out/$tag; mkdir -p "$h"; m=
    case $tag in
    m3) t=thumbv7m-none-eabi; H=EMBCC_THUMB_HARNESS; l=thumb
        "$EMBCC" --target=$t -O0 -c tests/harness/thumb/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t -O0 -c tests/harness/thumb/io.c -o "$h/io.o" ;;
    rv32*) t=riscv32-unknown-elf; H=EMBCC_RISCV_HARNESS; l=riscv
        [ $tag = rv32nc ] && m=-march=rv32ima
        "$EMBCC" --target=$t $m -O0 -c tests/harness/riscv/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t $m -O0 -c tests/harness/riscv/io.c -o "$h/io.o" ;;
    avr) t=avr; H=EMBCC_AVR_HARNESS; l=avr
        "$EMBCC" --target=$t -c tests/harness/avr/boot.S -o "$h/boot.o" &&
        "$EMBCC" --target=$t -Os -c tests/harness/avr/io.c -o "$h/io.o" &&
        sh tools/build-rt.sh avr "$h" > "$h/rt.log" 2>&1 ;;
    esac &&
    "$EMBCC" --target=$t $m -O1 -c "$src" -o "$h/prof.o" &&
    env "$H=$h" sh tests/harness/$l/link.sh "$out/$tag.elf" "$h/prof.o" > /dev/null 2>&1
}

# the report's row for a function: "SELF_I SELF_C INCL_I INCL_C CALLS"
row() {
    awk -v f="$2" '$NF == f && NF == 8 { print $1, $2, $4, $5, $7 }' "$1"
}

check() {
    tag=$1; isr=$2; vec=$3; shift 3
    f0=$fail
    build "$tag" || { echo "FAIL $tag: prof.c does not build"; fail=1; return; }
    e=$out/$tag.elf
    run="$EMBSIM $e $* --max-insns 1000000"
    $run --count "$out/$tag.plain.count" > "$out/$tag.plain.out" 2> /dev/null
    $run --count "$out/$tag.count" --stats --profile="$out/$tag.prof" \
        > "$out/$tag.out" 2> "$out/$tag.stats"
    $run --profile="$out/$tag.folded" --profile-format=collapsed > /dev/null 2>&1
    $run --profile="$out/$tag.ifolded" --profile-format=collapsed-insns > /dev/null 2>&1
    $run --trace "$out/$tag.trace" > /dev/null 2>&1
    cmp -s "$out/$tag.plain.out" "$out/$tag.out" &&
    cmp -s "$out/$tag.plain.count" "$out/$tag.count" || {
        echo "FAIL $tag: --profile changed the run (its output or --count)"; fail=1; }
    [ "$(tr -s ' \n' '  ' < "$out/$tag.out")" = "37 3 " ] || {
        echo "FAIL $tag: the program printed '$(cat "$out/$tag.out")', not 37 and 3 ticks"
        fail=1; return; }

    # the totals: --stats', the (total) row, the rows, the stacks
    set -- $(sed -n 's/.*; \([0-9]*\) instructions, \([0-9]*\) cycles (est.)$/\1 \2/p' "$out/$tag.stats")
    ti=${1:-x}; tc=${2:-y}
    tot=$(awk '$NF == "(total)" { print $1, $2 }' "$out/$tag.prof")
    sum=$(awk 'NF == 8 && $NF != "function" { i += $1; c += $2 } END { print i, c }' "$out/$tag.prof")
    fc=$(awk '{ s += $NF } END { print s }' "$out/$tag.folded")
    fi_=$(awk '{ s += $NF } END { print s }' "$out/$tag.ifolded")
    [ "$tot" = "$ti $tc" ] && [ "$sum" = "$ti $tc" ] && [ "$fc" = "$tc" ] &&
    [ "$fi_" = "$ti" ] || {
        echo "FAIL $tag: the counts do not add up to --stats' $ti instructions, $tc cycles:"
        echo "     | (total) $tot, the rows $sum, the stacks $fc cycles and $fi_ instructions"
        fail=1; }

    # the calls
    for fc_ in "a 3" "b 1" "leaf 6" "rec 5" "$isr 3"; do
        set -- $fc_
        got=$(row "$out/$tag.prof" "$1" | awk '{ print $5 }')
        [ "$got" = "$2" ] || { echo "FAIL $tag: $1 was called ${got:-no} times, not $2"; fail=1; }
    done

    # the paths, from main up
    sed 's/^.*;main;/main;/; s/ [0-9]*$//' "$out/$tag.ifolded" | grep '^main' | sort -u > "$out/$tag.paths"
    for p in "main;a;leaf" "main;b;leaf" "main;leaf" "main;rec;rec;rec;rec;rec" \
             "main;spin;$isr"; do
        grep -qx "$p" "$out/$tag.paths" || { echo "FAIL $tag: no path $p"; fail=1; }
    done
    bad=$(grep ";$isr" "$out/$tag.paths" | grep -vx "main;$isr" | grep -vx "main;spin;$isr")
    [ -z "$bad" ] || { echo "FAIL $tag: the handler on another path: $bad"; fail=1; }
    deep=$(awk -F';' 'NF > 9' "$out/$tag.folded" | head -1)
    [ -z "$deep" ] || { echo "FAIL $tag: a path deeper than prof.c makes: $deep"; fail=1; }

    # inclusive: the sum of the stacks a function is on
    for f in main a b rec leaf spin "$isr"; do
        want=$(awk -v f="$f" '{ n = split($1, p, ";"); for (i = 1; i <= n; i++)
                   if (p[i] == f) { s += $2; break } } END { print s + 0 }' "$out/$tag.ifolded")
        got=$(row "$out/$tag.prof" "$f" | awk '{ print $3 }')
        [ "$got" = "$want" ] || { echo "FAIL $tag: $f's inclusive instructions $got, its stacks' $want"; fail=1; }
    done

    # self: the trace's addresses, each in the function llvm-nm says
    if [ $have_nm = 1 ]; then
        "$NM" -S --defined-only "$e" | awk '$3 ~ /^[tT]$/ && NF == 4' > "$out/$tag.nm"
        for f in main a b rec leaf spin "$isr"; do
            v=-1; [ "$f" = "$isr" ] && v=$vec
            want=$(awk -v f="$f" -v vec="$v" '
                FNR == NR { if ($4 == f) { lo = strtonum_("0x" $1); hi = lo + strtonum_("0x" $2) } next }
                { a = strtonum_("0x" substr($1, 1, 8)); if ((a >= lo && a < hi) || a == vec) n++ }
                END { print n + 0 }
                function strtonum_(s,   i, c, v) { v = 0; s = tolower(s)
                    for (i = 3; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c }
                    return v - v % 2 }' "$out/$tag.nm" "$out/$tag.trace")
            got=$(row "$out/$tag.prof" "$f" | awk '{ print $1 }')
            [ "$got" = "$want" ] || { echo "FAIL $tag: $f ran $got instructions by the profile, $want by the trace"; fail=1; }
        done
    fi
    [ $fail = "$f0" ] && echo "  $tag: $ti instructions and $tc cycles accounted for; calls, paths, self and inclusive as prof.c makes them"
}

check m3 tick -1 --board lm3s6965evb
check rv32 tick -1 --board virt --ram-size 8M
check rv32nc tick -1 --board virt --ram-size 8M
check avr __vector_11 44 --board uno

# the M3's exceptions: exc.c counts its own SVCs, PendSVs and faults
x=$out/exc
if "$EMBCC" --target=thumbv7m-none-eabi -O2 -c tests/golden/embsim/exc.c -o "$x.o" &&
   sh tools/build-rt.sh thumbv7m-none-eabi "$out/rt" > "$out/rt.log" 2>&1 &&
   "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$x.o" "$out/rt/librt.a" -o "$x.elf" > /dev/null 2>&1; then
    "$EMBSIM" "$x.elf" --board lm3s6965evb --max-insns 10000000 --stats \
        --profile="$x.prof" > "$x.out" 2> "$x.stats"
    "$EMBSIM" "$x.elf" --board lm3s6965evb --max-insns 10000000 \
        --profile="$x.folded" --profile-format=collapsed > /dev/null 2>&1
    # two SVCs from the main stack, a third from the PSP ("psp" 1 1 3)
    grep -q '^svc 2$' "$x.out" && grep -q '^psp 113$' "$x.out" &&
    grep -q '^pendsv 1$' "$x.out" && grep -q '^faults 1 1$' "$x.out" ||
        { echo "FAIL exc: exc.c's own counts changed"; cat "$x.out"; fail=1; }
    for fc_ in "svc_handler 3" "pendsv 1" "usage_fault 1" "hard_fault 1"; do
        set -- $fc_
        got=$(row "$x.prof" "$1" | awk '{ print $5 }')
        [ "$got" = "$2" ] || { echo "FAIL exc: $1 entered ${got:-no} times, not $2"; fail=1; }
    done
    grep -q ';main;irq_low;irq_high [0-9]*$' "$x.folded" ||
        { echo "FAIL exc: irq_high is not nested in irq_low"; fail=1; }
    tc=$(sed -n 's/.*instructions, \([0-9]*\) cycles (est.)$/\1/p' "$x.stats")
    [ "$(awk '{ s += $NF } END { print s }' "$x.folded")" = "$tc" ] ||
        { echo "FAIL exc: the stacks do not add up to $tc cycles"; fail=1; }
    [ $fail = 0 ] && echo "  exc.c: SVC, PendSV, the faults entered as often as it counts; irq_high nested in irq_low"
else
    echo "FAIL exc: exc.c does not build"; fail=1
fi

"$EMBSIM" "$out/m3.elf" --profile --profile-format=svg > "$out/bad.out" 2>&1
[ $? = 2 ] && grep -q 'report, collapsed or collapsed-insns' "$out/bad.out" || {
    echo "FAIL: --profile-format=svg is not refused"; fail=1; }
[ $fail = 0 ] && echo "embsim --profile: every instruction and cycle accounted for, along the calls the cores made"
exit $fail
