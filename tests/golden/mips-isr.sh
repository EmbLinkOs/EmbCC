#!/bin/sh
# MIPS32 interrupt handlers: __attribute__((interrupt)) with GCC's (and
# clang's) arguments -- "eic", "vector=sw0".."vector=hw5" -- and GCC's
# keep_interrupts_masked.
#
# A handler is entered between two instructions of code with a value in
# every register, and runs with interrupts enabled again, so it saves EPC
# and Status as well as every register it changes, and returns with eret.
# Three checks:
#
#   THE BAD FORMS are refused by name: parameters, a result, an argument
#   that names no mode, keep_interrupts_masked alone, GCC's
#   use_shadow_register_set and use_debug_exception_return, MIPS64.
#
#   THE SHAPE, against clang and against the rule: each form's Status
#   sequence (the RIPL into IPL, IM0..IMn cleared, or IE with EXL), di and
#   ehb before EPC and Status come back, eret and no jr $ra. A handler
#   that calls saves exactly the registers clang saves -- at, v0-v1,
#   a0-a3, t0-t9, gp, ra, HI and LO -- and one that does not, exactly the
#   caller-saved registers its instructions write, and HI/LO exactly when
#   it multiplies or divides, read off llvm-objdump's disassembly.
#
#   THE RUN, on QEMU's malta (tests/golden/mips-isr/main.c): the CP0 timer
#   interrupts code that keeps a pattern in every caller-saved register and
#   HI/LO (torture.S), and a C computation, hundreds of times per phase --
#   a masked handler, one that calls, and with vectored interrupts a
#   software-interrupt handler the timer interrupts in turn. At -O0 to -Os.
#
# The plain form (the EIC default) copies Cause.RIPL into Status.IPL. The
# 24Kc on malta has no External Interrupt Controller, so those bits are its
# pending lines there, and a plain handler re-enters at once -- as GCC's
# would; it is checked for shape, and run in its vector= form.
set -u
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-isr T=mips-none-elf CT=mips-unknown-elf
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-isr T=mipsel-none-elf CT=mipsel-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_CLANG:-clang}
d=tests/golden/mips-isr
fail() { echo "FAIL: $*"; exit 1; }

# ---- the bad forms ----------------------------------------------------
refuse() {         # refuse TRIPLE 'source' 'expected text'
    printf '%s\n' "$2" > "$out/bad.c"
    if "$EMBCC" --target=$1 -c "$out/bad.c" -o "$out/bad.o" \
        2> "$out/bad.err"; then
        fail "$1 compiled: $2"
    fi
    grep -q "$3" "$out/bad.err" || {
        echo "$1: $2"; cat "$out/bad.err"; fail "not refused with '$3'"; }
}
refuse $T '__attribute__((interrupt)) void h(int x) { (void)x; }' \
    "interrupt handler 'h' takes parameters"
refuse $T '__attribute__((interrupt("vector=hw5"))) int h(void) { return 1; }' \
    "interrupt handler 'h' returns a value"
refuse $T '__attribute__((interrupt("vector=hw6"))) void h(void) { }' \
    'interrupt wants "eic" or "vector=sw0"'
refuse $T '__attribute__((interrupt("machine"))) void h(void) { }' \
    'interrupt wants "eic" or "vector=sw0"'
refuse $T '__attribute__((keep_interrupts_masked)) void h(void) { }' \
    'keep_interrupts_masked but not an interrupt handler'
refuse $T '__attribute__((interrupt, use_shadow_register_set)) void h(void) { }' \
    'use_shadow_register_set)) is not supported'
refuse $T '__attribute__((interrupt, use_debug_exception_return)) void h(void) { }' \
    'use_debug_exception_return)) is not supported'
refuse $T '__attribute__((interrupt)) void h(void); __attribute__((interrupt("vector=sw1"))) void h(void) { }' \
    'different kind of interrupt handler'
refuse mips64el-none-elf '__attribute__((interrupt)) void h(void) { }' \
    'interrupt)) is not supported'
echo "the bad forms are refused by name"

# ---- the shape --------------------------------------------------------
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "(SKIP: no $OBJDUMP for the shape)"; OBJDUMP=; }
cat > "$out/shape.c" <<'EOF'
volatile int cnt, v[8];
volatile long long ll;
void ext(void);
__attribute__((interrupt)) void eic(void) { cnt++; }
__attribute__((interrupt("eic"))) void eic2(void) { cnt += v[1] * v[2]; }
__attribute__((interrupt("vector=hw5"))) void hw5(void)
{
    int a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5];
    v[6] = a + b + c + d + e + f + (a ^ c) + (b - d) + (e | f);
}
__attribute__((interrupt("vector=sw0"))) void sw0(void) { ll = ll * cnt; cnt /= 7; }
__attribute__((interrupt, keep_interrupts_masked)) void masked(void) { cnt++; }
__attribute__((interrupt)) void calls(void) { ext(); cnt++; }
__attribute__((keep_interrupts_masked, interrupt("vector=hw0"))) void mcalls(void) { ext(); }
void late(void);
__attribute__((interrupt("vector=hw2"))) void late(void) { cnt += 2; }
/* past addiu's reach: made in two steps, the saves first */
__attribute__((interrupt, keep_interrupts_masked)) void big(void)
{
    volatile char buf[40000];
    buf[0] = 1;
    buf[39999] = 2;
    cnt += buf[0] + buf[39999];
}
EOF
# one function's lines, from its label to the next blank line
fn_body() { sed -n "/<$2>:/,/^\$/p" "$1"; }
# the GPRs it saves: the stores to sp after the Status mtc0 (EmbCC's saves
# follow it), k0/k1 left out, as numbers
saves() {
    fn_body "$1" "$2" | awk '
        /mtc0.*\$12/ { on = 1; next }
        on && $2 == "sw" && $4 ~ /\(\$sp\)$/ {
            r = $3; sub(/,/, "", r); print r; next }
        on { exit }' | sed 's/\$ra/$31/; s/\$gp/$28/; s/\$at/$1/; s/\$//' |
        grep -vxE '26|27' | sort -n | tr '\n' ' '
}
# ...and what its instructions write: the first operand of anything but a
# store, a transfer, a CP0 write, HI/LO's writes and the traps
writes() {
    fn_body "$1" "$2" | awk '
        NR == 1 { next }
        $2 ~ /^(sb|sh|sw|swl|swr|b|j|mtc0|mthi|mtlo|mult|multu|div|divu|madd|msub|teq|tne|tge|tlt|sync|eret|ehb|di|ei|nop|syscall|break|cache|pref)/ { next }
        { r = $3; sub(/,.*/, "", r); print r }' |
        sed 's/\$ra/$31/; s/\$gp/$28/; s/\$at/$1/; s/\$//' |
        grep -xE '([1-9]|1[0-5]|2[45]|31)' | sort -nu | tr '\n' ' '
}
hilo_saved() { fn_body "$1" "$2" | grep -qE 'mfhi[[:space:]]+\$26' && echo 1 || echo 0; }
hilo_used() {
    fn_body "$1" "$2" | grep -qE '[[:space:]](mult|multu|div|divu|madd|maddu|msub|msubu|mul|mthi|mtlo)[[:space:]]' &&
        echo 1 || echo 0
}
for O in -O0 -O1 -O2 -Os; do
    [ -n "$OBJDUMP" ] || break
    o=$out/shape$O
    "$EMBCC" --target=$T $O -c "$out/shape.c" -o "$o.o" ||
        fail "$O: shape.c did not compile"
    "$OBJDUMP" -d --no-show-raw-insn "$o.o" > "$o.dis"
    for fn in eic eic2 hw5 sw0 masked calls mcalls late big; do
        b=$(fn_body "$o.dis" $fn)
        has() { echo "$b" | grep -qE "[[:space:]]$1" || fail "$O: $fn has no '$1'"; }
        hasnt() { echo "$b" | grep -qE "[[:space:]]$1" && fail "$O: $fn has '$1'"; }
        case $fn in
        eic|eic2|calls)
            has 'mfc0[[:space:]]+\$26, \$13'
            has 'ext[[:space:]]+\$26, \$26, 0xa, 0x6'
            has 'ins[[:space:]]+\$27, \$26, 0xa, 0x6'
            has 'ins[[:space:]]+\$27, \$zero, 0x1, 0x4' ;;
        hw5) has 'ins[[:space:]]+\$27, \$zero, 0x8, 0x8'
             has 'ins[[:space:]]+\$27, \$zero, 0x1, 0x4' ;;
        sw0) has 'ins[[:space:]]+\$27, \$zero, 0x8, 0x1' ;;
        late) has 'ins[[:space:]]+\$27, \$zero, 0x8, 0x5' ;;
        masked|mcalls|big)
            has 'ins[[:space:]]+\$27, \$zero, 0x0, 0x5'
            hasnt 'mfc0[[:space:]]+\$26, \$13'
            hasnt 'di$' ;;
        esac
        case $fn in masked|mcalls|big) ;; *) has 'di$'; has 'ehb$';; esac
        has 'mfc0[[:space:]]+\$27, \$14'
        has 'mtc0[[:space:]]+\$27, \$14'
        has 'mtc0[[:space:]]+\$27, \$12'
        echo "$b" | grep -qE "[[:space:]]eret\$" || fail "$O: $fn does not return with eret"
        echo "$b" | grep -qE "[[:space:]]jr[[:space:]]+\\\$ra" && fail "$O: $fn returns with jr \$ra"
        got=$(saves "$o.dis" $fn)
        case $fn in
        calls|mcalls)
            # every caller-saved register, as clang saves them
            if command -v "$CLANG" >/dev/null 2>&1; then
                [ -f "$out/clang.s" ] ||
                    "$CLANG" --target=$CT -mcpu=mips32r2 -msoft-float \
                        -mno-abicalls -O2 -S -o "$out/clang.s" "$out/shape.c" \
                        2>/dev/null || fail "clang did not compile shape.c"
                cw=$(sed -n "/^$fn:/,/eret/p" "$out/clang.s" |
                     awk '$1 == "sw" && $3 ~ /\(\$sp\)$/ { r = $2; sub(/,/, "", r); print r }' |
                     sed 's/\$ra/$31/; s/\$gp/$28/; s/\$//' |
                     grep -vxE '26|27' | sort -n | tr '\n' ' ')
                case "$cw" in *31*) ;; *) fail "clang's $fn: '$cw' not read";; esac
                [ "$got" = "$cw" ] || { echo "embcc: $got"; echo "clang: $cw"
                    fail "$O: $fn does not save what clang saves"; }
                [ "$(hilo_saved "$o.dis" $fn)" = 1 ] || fail "$O: $fn does not save HI/LO"
            fi ;;
        *)
            wr=$(writes "$o.dis" $fn)
            [ "$got" = "$wr" ] || { echo "saves:  $got"; echo "writes: $wr"
                fail "$O: $fn does not save exactly what it writes"; }
            [ "$(hilo_saved "$o.dis" $fn)" = "$(hilo_used "$o.dis" $fn)" ] ||
                fail "$O: $fn saves HI/LO $(hilo_saved "$o.dis" $fn), uses them $(hilo_used "$o.dis" $fn)" ;;
        esac
    done
done
[ -n "$OBJDUMP" ] && echo "each form's Status sequence, di/ehb, eret; clang's set when calling, exactly what is written otherwise, HI/LO when used; -O0..-Os"

# ---- the run ----------------------------------------------------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "(SKIP: no $QEMU)"; exit 0; }
want="A 0 0 1 1
B 0 0 1 1
C 0 0 1 1"
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/mips/$f.c -o "$out/$f.o" ||
        fail "the harness does not compile"
done
"$EMBCC" --target=$T -c $d/torture.S -o "$out/torture.o" ||
    fail "torture.S does not assemble"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c $d/main.c -o "$out/main$O.o" ||
        fail "$O: main.c does not compile"
    sh tests/harness/mips/link.sh "$out/t$O.elf" "$out/main$O.o" \
        "$out/torture.o" || fail "$O: does not link"
    # -icount: Count counts instructions, so the interrupts land in the
    # same places however busy the machine is
    tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until "==EXIT [0-9]* ==" \
        "$QEMU" -M malta -cpu 24Kc -m 64 -display none -monitor none \
        -serial null -serial null -serial stdio -no-reboot -icount shift=0 \
        -kernel "$out/t$O.elf" 2>/dev/null | tr -d '\r' > "$out/t$O.out"
    got=$(head -3 "$out/t$O.out" | sed 's/ *$//')
    if [ "$got" != "$want" ] || ! grep -q '==EXIT 42 ==' "$out/t$O.out"; then
        echo "$O: wanted"; echo "$want"; echo "and ==EXIT 42, got:"
        head -c 400 "$out/t$O.out"; echo
        fail "$O: a handler changed the interrupted code's registers"
    fi
    set -- $(sed -n 4p "$out/t$O.out")
    [ "${6:-x}" = 0 ] || fail "$O: unexpected exceptions: $*"
    echo "  $O: interrupts per phase A B C = $1 $2 $3, software interrupts $4, of them interrupted by the timer $5"
done
echo "every caller-saved register and HI/LO survive hundreds of timer interrupts per phase: masked, calling, and nested handlers, -O0..-Os"
