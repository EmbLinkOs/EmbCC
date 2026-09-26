#!/bin/sh
# -fsanitize, in trap mode.
#
# There is only one mode here and that is deliberate: a DIAGNOSING
# sanitizer calls __ubsan_handle_* to print, and a bare metal target has
# nowhere to print to. So a failed check runs the target's trap
# instruction, which IR_UD2 already lowers on all four backends: `ud2`
# on x86-64, `udf #0` on aarch64 and ARMv7-M, `unimp` on RISC-V. Under a
# debugger that is a breakpoint at the offending operation; without one
# the program stops instead of continuing with a wrong value.
#
# Every check is ordinary IR -- a comparison and a branch -- so this
# tests three separate things, and the middle one is the one that bites:
#
#   1. each kind of UB traps,
#   2. code that is NOT undefined does not,
#   3. a check whose operands are known folds away at -O2.
#
# (2) is not padding. Three arithmetic sites in irgen lower a C operator,
# and converting two of them left signed `+` and `-` unchecked while `*`
# and `/` were checked -- caught only by running the cases. A later one
# is as easy to miss, so every operator that can overflow is listed.
set -u
echo "TEST-MARKER sanitize"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/sanitize-$ARCH
rm -rf "$out"; mkdir -p "$out"

# $OBJDUMP is the host's, which is x86-64's even in the aarch64 run, so
# the disassembly half needs the target's own.
case "$ARCH" in
    x86_64)  OD=x86_64-elf-objdump ;;
    aarch64) OD=aarch64-elf-objdump ;;
    *)       OD= ;;
esac

cat > "$out/ub.c" <<'CEOF'
#include <stdio.h>
/* volatile so nothing is folded at compile time: the check has to be
 * the thing that catches it, not the constant folder. */
volatile int imax = 2147483647, imin = (-2147483647 - 1);
volatile int zero = 0, minus1 = -1, big = 40, neg = -3, one = 1, two = 2;
int main(void)
{
    volatile int r = 0;
#if   CASE==0
    r = imax + 1;                       /* signed overflow, +        */
#elif CASE==1
    r = imin - 1;                       /* signed overflow, -        */
#elif CASE==2
    r = imax * two;                     /* signed overflow, *        */
#elif CASE==3
    r = -imin;                          /* signed overflow, unary -  */
#elif CASE==4
    { int x = imax; x += one; r = x; }  /* signed overflow, +=       */
#elif CASE==5
    { int x = imax; x++;      r = x; }  /* signed overflow, ++       */
#elif CASE==6
    { int x = imin; --x;      r = x; }  /* signed overflow, --       */
#elif CASE==7
    r = one / zero;                     /* divide by zero            */
#elif CASE==8
    r = one % zero;                     /* modulo by zero            */
#elif CASE==9
    r = imin / minus1;                  /* INT_MIN / -1              */
#elif CASE==10
    r = one << big;                     /* shift count >= width      */
#elif CASE==11
    r = one << neg;                     /* negative shift count      */
#elif CASE==12
    { int x = one; x <<= big; r = x; }  /* shift count, <<=          */

/* --- and everything below is DEFINED, so none of it may trap --- */
#elif CASE==13
    r = imax - one;
#elif CASE==14
    r = imin / two;                     /* only /-1 is undefined     */
#elif CASE==15
    r = minus1 * two;
#elif CASE==16
    r = big >> two;
#elif CASE==17
    { unsigned u = 0xffffffffu; u += one; r = (int)u; }  /* wraps, defined */
#elif CASE==18
    { unsigned u = 0; u--; r = (int)(u >> 28); }          /* wraps, defined */
#elif CASE==19
    { unsigned char c = 255; c++; r = c; }                /* wraps, defined */
#elif CASE==20
    { int a[4] = {1,2,3,4}; int *p = a; p++; p--; r = *p; } /* pointer ++ */
#elif CASE==21
    { long n = 0; for (int i = 0; i < 1000; i++) n += i; r = (int)n; }
#elif CASE==22
    r = one << 31;                      /* in range for a 32-bit shift */
#endif
    printf("%d\n", r);
    return 0;
}
CEOF

# 0..12 are undefined and must trap; 13..22 are defined and must not.
LAST_UB=12
LAST=22
for opt in -O0 -O1; do
    k=0
    while [ "$k" -le "$LAST" ]; do
        "$EMBCC" --target="$TARGET" $opt -fsanitize=undefined -DCASE=$k \
            -c "$out/ub.c" -o "$out/u.o" 2> "$out/cc.log" || {
            echo "case $k $opt: does not compile:"; head -3 "$out/cc.log"
            exit 1; }
        t_link "$out/u" "$out/u.o" > "$out/ln.log" 2>&1 || {
            echo "case $k $opt: does not link:"; head -3 "$out/ln.log"
            exit 1; }
        if t_run "$out/u" > "$out/run.log" 2>&1; then rc=0; else rc=1; fi
        if [ "$k" -le "$LAST_UB" ]; then
            [ "$rc" = 0 ] && {
                echo "case $k $opt: undefined behaviour did NOT trap"
                echo "  (it printed: $(head -1 "$out/run.log"))"; exit 1; }
        else
            [ "$rc" = 0 ] || {
                echo "case $k $opt: defined behaviour trapped"
                head -3 "$out/run.log"; exit 1; }
        fi
        k=$((k + 1))
    done
done
echo "thirteen kinds of undefined behaviour trap, and ten defined ones do
not, at -O0 and -O1"

# ---- and without -fsanitize, none of it traps -------------------------
# The checks must be OFF by default: a compiler that traps on overflow
# without being asked is a different language.
k=0
while [ "$k" -le "$LAST_UB" ]; do
    "$EMBCC" --target="$TARGET" -O1 -DCASE=$k -c "$out/ub.c" -o "$out/n.o" \
        2>/dev/null || { echo "case $k: does not compile without -fsanitize"
                         exit 1; }
    t_link "$out/n" "$out/n.o" > /dev/null 2>&1 || {
        echo "case $k: does not link without -fsanitize"; exit 1; }
    # The three division cases (7, 8, 9) are skipped: on x86-64 an
    # integer divide by zero and INT_MIN / -1 both raise #DE in the
    # HARDWARE, sanitizer or not, so "it died" says nothing about
    # whether a check was inserted. On aarch64 the same code returns 0
    # and does not fault, so the case is not portable either way. What
    # this loop is really asking -- that no check was added when none
    # was asked for -- is answered by the other ten.
    if [ "$k" != 7 ] && [ "$k" != 8 ] && [ "$k" != 9 ]; then
        t_run "$out/n" > /dev/null 2>&1 || {
            echo "case $k: trapped WITHOUT -fsanitize"; exit 1; }
    fi
    k=$((k + 1))
done
echo "and none of them traps when -fsanitize was not asked for"

# ---- a check the optimizer can settle leaves nothing behind -----------
# This is the reason the checks are IR and not a per-backend pattern.
if [ -n "$OD" ] && command -v "$OD" > /dev/null 2>&1; then
printf 'int f(int a){ return a / 7; }\nint g(int a){ return a << 3; }\n' \
    > "$out/k.c"
"$EMBCC" --target="$TARGET" -O2 -fsanitize=undefined -c "$out/k.c" \
    -o "$out/k.o" || { echo "the constant-operand file does not compile"
                       exit 1; }
n=$("$OD" -d "$out/k.o" | grep -cE '\bud2\b|\budf\b|unimp' || true)
[ "$n" = 0 ] || {
    echo "a divide by 7 and a shift by 3 left $n trap(s) at -O2"
    "$OD" -d "$out/k.o" | head -20; exit 1; }
# ...while a runtime divisor still has its check.
printf 'int f(int a, int b){ return a / b; }\n' > "$out/r.c"
"$EMBCC" --target="$TARGET" -O2 -fsanitize=undefined -c "$out/r.c" \
    -o "$out/r.o" || { echo "the runtime-divisor file does not compile"; exit 1; }
n=$("$OD" -d "$out/r.o" | grep -cE '\bud2\b|\budf\b|unimp' || true)
[ "$n" -ge 1 ] || {
    echo "a divide by a runtime value has no check at -O2"; exit 1; }
echo "at -O2 a constant divisor and a constant shift leave no check, and a
runtime divisor keeps one"
else
    echo "SKIP the -O2 folding half: no objdump for $ARCH"
fi

# ---- the bare-metal targets, on QEMU ----------------------------------
# The whole point of trap mode is the board, so the traps are run there
# too. There is no exit status to read: a trapped program prints
# nothing, and a defined one prints its answer and ==END==.
cat > "$out/bm.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);
volatile int imax = 2147483647, zero = 0, one = 1, big = 40;
int main(void)
{
    volatile int r = 0;
#if   CASE==0
    r = imax + one;        /* signed overflow */
#elif CASE==1
    r = one / zero;        /* divide by zero  */
#elif CASE==2
    r = one << big;        /* bad shift count */
#else
    r = imax - one;        /* defined         */
#endif
    putn(r);
    puts_("\n==END==\n");
    return 0;
}
CEOF

bm_run() {   # bm_run NAME BUILD_TARGET LINK RUN [RUN_ARG]
    name=$1; tgt=$2; lnk=$3; run=$4; rarg=${5-}
    for k in 0 1 2 3; do
        "$EMBCC" --target="$tgt" -O1 -fsanitize=undefined -DCASE=$k \
            -c "$out/bm.c" -o "$out/bm.o" ||
            { echo "$name case $k: does not compile"; exit 1; }
        sh "$lnk" "$out/bm.elf" "$out/bm.o" > /dev/null 2>&1 ||
            { echo "$name case $k: does not link"; exit 1; }
        got=$(sh "$run" "$out/bm.elf" $rarg 2>&1 | tr -d '\n')
        case "$k" in
        3) case "$got" in *2147483646*==END==*) ;;
           *) echo "$name case 3: defined arithmetic did not run: '$got'"
              exit 1 ;; esac ;;
        *) case "$got" in *==END==*)
              echo "$name case $k: undefined behaviour did NOT trap"
              exit 1 ;; esac ;;
        esac
    done
    echo "$name: overflow, divide-by-zero and a bad shift each trap; the
defined case still runs"
}

for w in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$w}
    if command -v "$QEMU" > /dev/null 2>&1; then
        d="$out/rv$w"; mkdir -p "$d"
        # $out is already absolute, so $d is too -- no $PWD prefix.
        EMBCC_RISCV_HARNESS="$d"; export EMBCC_RISCV_HARNESS
        for f in boot io; do
            "$EMBCC" --target=riscv$w-unknown-elf -c \
                "$EMBCC_ROOT/tests/harness/riscv/$f.c" -o "$d/$f.o" ||
                { echo "rv$w: the harness does not compile"; exit 1; }
        done
        # riscv/run.sh takes the XLEN as its second argument.
        bm_run "rv$w" "riscv$w-unknown-elf" \
               "$EMBCC_ROOT/tests/harness/riscv/link.sh" \
               "$EMBCC_ROOT/tests/harness/riscv/run.sh" "$w"
    else
        echo "SKIP rv$w: $QEMU absent"
    fi
done

QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
if command -v "$QEMU" > /dev/null 2>&1; then
    d="$out/thumb"; mkdir -p "$d"
    EMBCC_THUMB_HARNESS="$d"; export EMBCC_THUMB_HARNESS
    for f in boot io; do
        "$EMBCC" --target=thumbv7m-none-eabi -c \
            "$EMBCC_ROOT/tests/harness/thumb/$f.c" -o "$d/$f.o" ||
            { echo "thumb: the harness does not compile"; exit 1; }
    done
    bm_run "thumb" "thumbv7m-none-eabi" \
           "$EMBCC_ROOT/tests/harness/thumb/link.sh" \
           "$EMBCC_ROOT/tests/harness/thumb/run.sh"
else
    echo "SKIP thumb: $QEMU absent"
fi

# ---- what it REFUSES --------------------------------------------------
# A sanitizer that needs a runtime is refused BY NAME, not silently
# dropped from the set -- "I asked for address and got nothing" is the
# failure this is all meant to prevent (THE RULE).
for s in address thread memory leak bounds object-size; do
    if "$EMBCC" --target="$TARGET" -fsanitize=$s -c "$out/r.c" -o /dev/null \
         2> "$out/s.err"; then
        echo "-fsanitize=$s was accepted, and there is no runtime for it"
        exit 1
    fi
    grep -q "fsanitize=$s is not supported" "$out/s.err" || {
        echo "the refusal does not name $s:"; head -2 "$out/s.err"; exit 1; }
done
# The spellings that mean "trap" are accepted, since trap is the only mode.
for f in -fsanitize-trap=undefined -fsanitize-undefined-trap-on-error; do
    "$EMBCC" --target="$TARGET" $f -fsanitize=undefined -c "$out/r.c" \
        -o /dev/null 2> "$out/t.err" || {
        echo "$f was refused:"; head -2 "$out/t.err"; exit 1; }
done
# And a check can be switched back off by name.
"$EMBCC" --target="$TARGET" -O1 -fsanitize=undefined -fno-sanitize=shift \
    -c "$out/k.c" -o /dev/null 2> "$out/no.err" || {
    echo "-fno-sanitize=shift was refused:"; head -2 "$out/no.err"; exit 1; }
echo "six sanitizers that need a runtime are refused by name; the trap
spellings and -fno-sanitize= are accepted"
