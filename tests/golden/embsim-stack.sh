#!/bin/sh
# embsim --stack-report and --stack-limit: how deep the stacks went, by
# function, against what the compiler and embrt say; and an overflow
# stopped where it happens.
#
#  1. tests/golden/embsim-an/prof.c (calls, recursion, an interrupt) at
#     -O1 -fstack-usage on the Cortex-M3, RV32 and the AVR, with the
#     harness's own .su files:
#       - each function's measured frame is its .su frame, exactly --
#         the harness's too: on the AVR io.c's putn, whose `push r31` /
#         `pop r31` around an immediate loaded into a low register is a
#         byte of its frame (on the AVR but the interrupt handler: see
#         below);
#       - the stack's depth is the sum of the .su frames along the
#         deepest path prof.c has, and every function's depth is at most
#         it;
#       - the inclusive depth of a, b and putn is embrt's bound for each
#         (the run takes each one's worst path), main has none (rec is
#         recursive), and the report says the run stayed within the
#         static numbers;
#       - the run is the same with --stack-report as without.
#  2. tests/golden/embsim-an/ovf.c recursing with 64-byte frames:
#       - linked with limit.ld, whose __stack_limit is 2 KiB below the
#         top: the run ends at the push that crosses it, status 3, naming
#         deep and ovf.c's line;
#       - --stack-limit ADDR, and a symbol for it;
#       - in the harness, with no limit: into .bss and .data (_end) on
#         the M3 and on the AVR's 2 KiB;
#       - shallow, no overflow: the run ends as it would.
#  3. tests/golden/embsim/exc.c: a thread on the PSP takes an SVC, whose
#     eight-word frame is the process stack's 32 bytes.
# On the AVR, EmbCC's .su file understates a frame, which the report
# flags above the static bound and this test leaves out of the .su
# comparison (a finding for the compiler): a signal handler's leaves out
# the registers its prologue saves (r0, SREG, r1, r18-r27, r30, r31).
set -u
echo "TEST-MARKER embsim-stack"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
EMBRT=${EMBRT:-$PWD/embrt}
export EMBLD
out=tests/golden/out/embsim-stack
rm -rf "${out:?}"; mkdir -p "$out"
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }
have_rt=0
[ -x "$EMBRT" ] && have_rt=1

# harness TAG: boot and io, with their .su files
harness() {
    tag=$1; h=$out/$tag; mkdir -p "$h"
    case $tag in
    m3) t=thumbv7m-none-eabi
        "$EMBCC" --target=$t -O0 -fstack-usage -fcallgraph-info=su -c tests/harness/thumb/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t -O0 -fstack-usage -fcallgraph-info=su -c tests/harness/thumb/io.c -o "$h/io.o" ;;
    rv32) t=riscv32-unknown-elf
        "$EMBCC" --target=$t -O0 -fstack-usage -fcallgraph-info=su -c tests/harness/riscv/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t -O0 -fstack-usage -fcallgraph-info=su -c tests/harness/riscv/io.c -o "$h/io.o" ;;
    avr) t=avr
        "$EMBCC" --target=$t -c tests/harness/avr/boot.S -o "$h/boot.o" &&
        "$EMBCC" --target=$t -Os -fstack-usage -c tests/harness/avr/io.c -o "$h/io.o" &&
        sh tools/build-rt.sh avr "$h" > "$h/rt.log" 2>&1 ;;
    esac
}

link() {
    case $1 in
    m3) H=EMBCC_THUMB_HARNESS; l=thumb ;;
    rv32) H=EMBCC_RISCV_HARNESS; l=riscv ;;
    avr) H=EMBCC_AVR_HARNESS; l=avr ;;
    esac
    env "$H=$out/$1" sh tests/harness/$l/link.sh "$2" "$3" > /dev/null 2>&1
}

# su FILE...: "function bytes" from .su files
su() {
    cat "$@" 2> /dev/null | awk -F'\t' '{ n = split($1, p, ":"); print p[n], $2 }'
}

# the report's row: "deepest depth frame su incl embrt"
row() {
    awk -v f="$2" '/^  / && $1 == f && NF >= 7 { print $2, $3, $4, $5, $6, $7 }' "$1"
}

profile_stack() {
    tag=$1; isr=$2; shift 2
    f0=$fail
    case $tag in
    m3) t=thumbv7m-none-eabi; m= ;;
    rv32) t=riscv32-unknown-elf; m= ;;
    avr) t=avr; m= ;;
    esac
    h=$out/$tag
    harness "$tag" &&
    "$EMBCC" --target=$t -O1 -fstack-usage -fcallgraph-info=su -c tests/golden/embsim-an/prof.c -o "$h/prof.o" &&
    link "$tag" "$out/$tag.elf" "$h/prof.o" || {
        echo "FAIL $tag: prof.c does not build"; fail=1; return; }
    sus=$(ls "$h"/*.su)
    rtarg=
    if [ $have_rt = 1 ] && [ $tag != avr ]; then
        "$EMBRT" "$h/boot.o" "$h/io.o" "$h/prof.o" --entry main --entry a \
            --entry b --entry putn --isr "$isr" --json \
            > "$out/$tag.embrt.json" 2> /dev/null
        rtarg="--stack-embrt $out/$tag.embrt.json"
    fi
    e=$out/$tag.elf
    "$EMBSIM" "$e" "$@" --max-insns 1000000 --count "$out/$tag.plain.count" > "$out/$tag.plain.out" 2>&1
    # shellcheck disable=SC2086
    "$EMBSIM" "$e" "$@" --max-insns 1000000 --count "$out/$tag.count" \
        --stack-report="$out/$tag.stack" $rtarg $sus > "$out/$tag.out" 2>&1
    cmp -s "$out/$tag.plain.out" "$out/$tag.out" &&
    cmp -s "$out/$tag.plain.count" "$out/$tag.count" || {
        echo "FAIL $tag: --stack-report changed the run (its output or --count)"; fail=1; }

    # each function's frame is its .su frame
    su $sus > "$out/$tag.su"
    nsu=0
    while read -r fn bytes; do
        set -- $(row "$out/$tag.stack" "$fn")
        [ $# = 6 ] || continue          # never ran
        nsu=$((nsu + 1))
        if [ "$fn" = "$isr" ] && [ $tag = avr ]; then
            [ "$3" -gt "$bytes" ] || { echo "FAIL avr: $fn's frame $3, not above its .su $bytes (has EmbCC's .su been fixed? update this test)"; fail=1; }
            continue
        fi
        [ "$3" = "$bytes" ] || { echo "FAIL $tag: $fn's frame was $3 bytes, its .su says $bytes"; fail=1; }
    done < "$out/$tag.su"
    # every function that ran and has a .su line was compared: prof.c's
    # six, the handler, and the harness's putn and writec at least
    [ $nsu -ge 9 ] || { echo "FAIL $tag: only $nsu functions' frames compared"; fail=1; }
    for fn in main a b rec leaf $isr putn writec; do
        [ -n "$(row "$out/$tag.stack" "$fn")" ] || { echo "FAIL $tag: no row for $fn"; fail=1; }
    done

    # the depth: the deepest path's .su frames, and no function deeper
    fr() { awk -v f="$1" '$1 == f { print $2 }' "$out/$tag.su"; }
    case $tag in
    m3) want=$(( $(fr reset) + $(fr main) + $(fr putn) + $(fr writec) )) ;;
    rv32) want=$(( $(fr _start) + $(fr main) + 5 * $(fr rec) )) ;;
    avr) want=$(( $(fr main) + $(fr spin) + 31 )) ;;   # the handler: 31, as measured
    esac
    got=$(sed -n 's/.*: \([0-9]*\) bytes used.*/\1/p' "$out/$tag.stack" | head -1)
    [ "$got" = "$want" ] || { echo "FAIL $tag: the stack went $got bytes deep; the deepest path's frames add up to $want"; fail=1; }
    worst=$(awk 'NF >= 7 && $3 ~ /^[0-9]+$/ { if ($3 > m) m = $3 } END { print m + 0 }' "$out/$tag.stack")
    [ "$worst" = "$got" ] || { echo "FAIL $tag: the deepest function is $worst bytes deep, the stack $got"; fail=1; }

    # the inclusive depths: embrt's bounds, reached; main's unbounded
    if [ -n "$rtarg" ]; then
        for fn in a b putn; do
            set -- $(row "$out/$tag.stack" $fn)
            [ "$5" = "$6" ] || { echo "FAIL $tag: $fn went $5 bytes deep, embrt bounds it at $6"; fail=1; }
        done
        set -- $(row "$out/$tag.stack" main)
        [ "$6" = none ] || { echo "FAIL $tag: main's embrt bound is '$6', not none (recursion)"; fail=1; }
    fi
    # the interrupt that came in spin is not spin's: its depth is its frame
    set -- $(row "$out/$tag.stack" spin)
    [ "$5" = "$3" ] || { echo "FAIL $tag: spin's inclusive depth $5 counts the handler (its frame is $3)"; fail=1; }
    if [ -n "$rtarg" ]; then
        grep -q '^the run stayed within the static numbers$' "$out/$tag.stack" ||
            { echo "FAIL $tag: the report does not say the run stayed within the static numbers"; fail=1; }
    fi
    bound=
    [ -n "$rtarg" ] && bound=", a, b and putn at the bounds embrt gives"
    [ $fail = "$f0" ] && echo "  $tag: frames as the .su files say, $got bytes deep as the deepest path's frames$bound"
}

profile_stack m3 tick --board lm3s6965evb
profile_stack rv32 tick --board virt --ram-size 8M
profile_stack avr __vector_11 --board uno

# ---- 2. overflows ----------------------------------------------------------
ovf() {   # TAG DEPTH: ovf.c into $out/TAG-ovf-DEPTH.o
    case $1 in
    m3) t=thumbv7m-none-eabi ;;
    avr) t=avr ;;
    esac
    "$EMBCC" --target=$t -O1 -g -DDEPTH="$2" -c tests/golden/embsim-an/ovf.c -o "$out/$1-ovf-$2.o"
}
f0=$fail
ovf m3 100 && "$EMBLD" -e reset -T tests/golden/embsim-an/limit.ld "$out/m3/boot.o" \
    "$out/m3/io.o" "$out/m3-ovf-100.o" -o "$out/limit.elf" > /dev/null 2>&1 ||
    { echo "FAIL: ovf.c does not link with limit.ld"; fail=1; }
"$EMBSIM" "$out/limit.elf" --max-insns 1000000 --stack-report="$out/limit.stack" > "$out/limit.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'stack overflow: sp 0x2000f7f8 is below the stack limit 0x2000f800 (__stack_limit), at deep (ovf.c:9)' "$out/limit.out" &&
    grep -q '^OVERFLOW: sp 0x2000f7f8 went below 0x2000f800 (__stack_limit), at deep (ovf.c:9)$' "$out/limit.stack" || {
    echo "FAIL: the overflow past __stack_limit (status $st):"; sed 's/^/     | /' "$out/limit.out" | head -5; fail=1; }
"$EMBSIM" "$out/limit.elf" --max-insns 1000000 --stack-limit 0x2000fc00 > "$out/limit2.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'below the stack limit 0x2000fc00 (--stack-limit), at deep' "$out/limit2.out" || {
    echo "FAIL: --stack-limit 0x2000fc00 (status $st):"; sed 's/^/     | /' "$out/limit2.out" | head -3; fail=1; }
"$EMBSIM" "$out/limit.elf" --max-insns 1000000 --stack-limit __stack_limit > "$out/limit3.out" 2>&1; st=$?
[ $st = 3 ] && grep -q 'below the stack limit 0x2000f800 (--stack-limit), at deep' "$out/limit3.out" || {
    echo "FAIL: --stack-limit __stack_limit (status $st)"; fail=1; }
"$EMBSIM" "$out/limit.elf" --stack-limit nosuchsymbol > "$out/limit4.out" 2>&1; st=$?
[ $st = 2 ] && grep -q "'nosuchsymbol' is neither an address nor a symbol" "$out/limit4.out" || {
    echo "FAIL: --stack-limit with an unknown symbol is not refused"; fail=1; }
# into .bss: the M3's 64 KiB and the AVR's 2 KiB, in their harnesses
for cfg in "m3|2000|--board lm3s6965evb" "avr|100|--board uno"; do
    IFS='|' read -r tag d args <<EOF
$cfg
EOF
    ovf "$tag" "$d" && link "$tag" "$out/$tag-bss.elf" "$out/$tag-ovf-$d.o" || {
        echo "FAIL $tag: ovf.c does not build"; fail=1; continue; }
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/$tag-bss.elf" $args --max-insns 10000000 --stack-report \
        > "$out/$tag-bss.out" 2>&1; st=$?
    [ $st = 3 ] && grep -q 'stack overflow: sp 0x[0-9a-f]* is below the end of .data and .bss 0x[0-9a-f]* (_end), at deep' "$out/$tag-bss.out" ||
        { echo "FAIL $tag: an overflow into .bss (status $st):"; sed 's/^/     | /' "$out/$tag-bss.out" | head -4; fail=1; }
    end=$(sed -n 's/.*below the end of .data and .bss 0x\([0-9a-f]*\) (_end).*/\1/p' "$out/$tag-bss.out" | head -1)
    sp=$(sed -n 's/.*stack overflow: sp 0x\([0-9a-f]*\) is below.*/\1/p' "$out/$tag-bss.out" | head -1)
    # the run stops at the first push past the end: one frame (72 bytes) at most below it
    [ -n "$end" ] && [ -n "$sp" ] && [ $((0x$end - 0x$sp)) -gt 0 ] && [ $((0x$end - 0x$sp)) -le 72 ] ||
        { echo "FAIL $tag: the overflow was not stopped at the first push past _end (sp $sp, _end $end)"; fail=1; }
done
ovf m3 5 && link m3 "$out/shallow.elf" "$out/m3-ovf-5.o" &&
"$EMBSIM" "$out/shallow.elf" --max-insns 1000000 --stack-report > "$out/shallow.out" 2>&1
grep -q 'stack overflow' "$out/shallow.out" && { echo "FAIL: deep(5) reported an overflow"; fail=1; }
grep -q '^3 embsim: lockup: a fault with HardFault already active' "$out/shallow.out" ||
    { echo "FAIL: deep(5) did not run to the harness's end"; sed 's/^/     | /' "$out/shallow.out" | head -3; fail=1; }
[ $fail = "$f0" ] && echo "  overflows: past __stack_limit, --stack-limit (an address and a symbol), into .bss on the M3 and the AVR, each stopped at the push that crossed"

# ---- 3. the PSP --------------------------------------------------------------
x=$out/exc
if "$EMBCC" --target=thumbv7m-none-eabi -O2 -c tests/golden/embsim/exc.c -o "$x.o" &&
   sh tools/build-rt.sh thumbv7m-none-eabi "$out/rt" > "$out/rt.log" 2>&1 &&
   "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$x.o" "$out/rt/librt.a" -o "$x.elf" > /dev/null 2>&1; then
    "$EMBSIM" "$x.elf" --board lm3s6965evb --max-insns 10000000 --stack-report="$x.stack" > /dev/null 2>&1
    grep -q '^process stack (PSP): top 0x[0-9a-f]*, deepest 0x[0-9a-f]*: 32 bytes used$' "$x.stack" ||
        { echo "FAIL exc: the PSP's 32 bytes (an SVC's frame)"; grep PSP "$x.stack"; fail=1; }
    grep -q '^main stack (MSP): top 0x20004000,' "$x.stack" ||
        { echo "FAIL exc: the MSP's top is not exc.c's vector table's 0x20004000"; fail=1; }
    [ $fail = 0 ] && echo "  exc.c: the PSP's 32 bytes, the MSP's from its reset value"
else
    echo "FAIL exc: exc.c does not build"; fail=1
fi

"$EMBSIM" "$out/m3.elf" --stack-su x.su > "$out/bad.out" 2>&1
[ $? = 2 ] && grep -q 'are for --stack-report' "$out/bad.out" || {
    echo "FAIL: --stack-su without --stack-report is not refused"; fail=1; }
[ $fail = 0 ] && echo "embsim --stack-report: frames, depths and bounds as the compiler and embrt say; overflows stopped where they happen"
exit $fail
