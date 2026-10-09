#!/bin/sh
# embsim --fault-report: a fault, decoded, when the core takes it.
#
# tests/golden/embsim-an/fault.c takes one fault in each MODE, at the end
# of main -> level1 -> level2 -> crash, built -O1 -g in the harness:
#   M3 1  a BusFault, handled: PRECISERR at 0x30000000, BFAR, the ldr,
#         the backtrace by .debug_frame to the marked lines, the handler
#   M3 2  a divide by zero escalated to a HardFault with no handler:
#         FORCED, DIVBYZERO, the divide, "handler: none", the lockup
#   M3 3  the BusFault in the SysTick handler: the backtrace goes through
#         the exception's frame into the code it interrupted
#   M3 1  without -g: the backtrace of the calls EmbSim saw
#   RV32 1  a load access fault with mtvec 0: mcause, mtval, the lw, the
#           backtrace, "handler: none", the lockup
#   RV32 2  an illegal instruction, handled: mtval holds its bits
# Each faulting instruction's mnemonic must be llvm-objdump's. Without
# --fault-report every run must be what it was: stdout, stderr, status
# and --count. tests/golden/embsim/exc.c, which takes a UsageFault and
# one escalated to HardFault and handles both, must get two reports; and
# an AVR instruction the part lacks, a lockup with no fault, one line and
# the calls.
set -u
echo "TEST-MARKER embsim-fault"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
OBJDUMP_=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
NM=${EMBCC_LLVM_NM:-llvm-nm}
export EMBLD
out=tests/golden/out/embsim-fault
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/embsim-an/fault.c
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }
have_od=0
command -v "$OBJDUMP_" >/dev/null 2>&1 && command -v "$NM" >/dev/null 2>&1 && have_od=1

line() { grep -n "/\* $1 \*/" "$src" | cut -d: -f1; }
LOAD=$(line LOAD); DIV=$(line DIV); CSR=$(line CSR)
CALL2=$(line CALL2); CALL1=$(line CALL1); CALL0=$(line CALL0)

# the harnesses
h=$out/m3; mkdir -p "$h"
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/boot.c -o "$h/boot.o" &&
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/io.c -o "$h/io.o" ||
    { echo "FAIL: the M3 harness does not build"; exit 1; }
h=$out/rv32; mkdir -p "$h"
"$EMBCC" --target=riscv32-unknown-elf -march=rv32ima -O0 -c tests/harness/riscv/boot.c -o "$h/boot.o" &&
"$EMBCC" --target=riscv32-unknown-elf -march=rv32ima -O0 -c tests/harness/riscv/io.c -o "$h/io.o" ||
    { echo "FAIL: the RV32 harness does not build"; exit 1; }

# build TAG ARCH MODE [-g]: $out/TAG.elf
build() {
    tag=$1; arch=$2; mode=$3; g=${4:-}
    case $arch in
    m3) t="--target=thumbv7m-none-eabi"; H=EMBCC_THUMB_HARNESS; l=thumb ;;
    rv32) t="--target=riscv32-unknown-elf -march=rv32ima"; H=EMBCC_RISCV_HARNESS; l=riscv ;;
    esac
    # shellcheck disable=SC2086
    "$EMBCC" $t -O1 $g -DMODE="$mode" -c "$src" -o "$out/$tag.o" &&
    env "$H=$out/$arch" sh tests/harness/$l/link.sh "$out/$tag.elf" "$out/$tag.o" > /dev/null 2>&1
}

# run TAG ARGS...: with and without --fault-report; the run must be the
# same, and the report goes to TAG.rep
run() {
    tag=$1; shift
    "$EMBSIM" "$out/$tag.elf" "$@" --max-insns 1000000 --count "$out/$tag.c0" \
        > "$out/$tag.o0" 2> "$out/$tag.e0"; s0=$?
    "$EMBSIM" "$out/$tag.elf" "$@" --max-insns 1000000 --count "$out/$tag.c1" \
        --fault-report="$out/$tag.rep" > "$out/$tag.o1" 2> "$out/$tag.e1"; s1=$?
    st=$s1
    [ $s0 = $s1 ] && cmp -s "$out/$tag.o0" "$out/$tag.o1" &&
    cmp -s "$out/$tag.e0" "$out/$tag.e1" && cmp -s "$out/$tag.c0" "$out/$tag.c1" ||
        { echo "FAIL $tag: --fault-report changed the run"; fail=1; }
}

# has TAG TEXT: the report has the line (fixed string)
has() {
    grep -qF -- "$2" "$out/$1.rep" || { echo "FAIL $1: no '$2' in the report:"
        sed 's/^/     | /' "$out/$1.rep" | head -30; fail=1; }
}

# bt TAG FN:LINE...: the backtrace's frames, in order, from #0
bt() {
    tag=$1; shift
    got=$(awk '/^    #[0-9]/ { f = $4; sub(/\+0x[0-9a-f]*$/, "", f); l = $5
               gsub(/[()]/, "", l); sub(/^.*:/, "", l); printf "%s:%s ", f, l }' "$out/$tag.rep")
    want="$* "
    case $got in
    "$want"*) ;;
    *) echo "FAIL $tag: the backtrace is '$got', not '$want'"; fail=1 ;;
    esac
}

# mnem TAG: the faulting instruction's mnemonic is llvm-objdump's
mnem() {
    [ $have_od = 1 ] || return 0
    tag=$1; triple=$2
    pc=$(sed -n 's/^  instruction 0x\([0-9a-f]*\): .*/\1/p' "$out/$tag.rep" | head -1)
    mine=$(sed -n 's/^  instruction 0x[0-9a-f]*: [0-9a-f ]*  *\([a-z.]*\) .*/\1/p' "$out/$tag.rep" | head -1)
    case $triple in
    riscv*) od="-M no-aliases --mattr=+m,+a" ;;
    *) od= ;;
    esac
    # shellcheck disable=SC2086
    ref=$("$OBJDUMP_" -d $od --triple="$triple" \
              --start-address=0x"$pc" --stop-address=$((0x$pc + 4)) "$out/$tag.elf" 2> /dev/null |
          awk '/^ *[0-9a-f]+:/ { for (i = 2; i <= NF; i++)
                   if ($i !~ /^[0-9a-f]+$/ || (length($i) != 4 && length($i) != 8)) break
                 print $i; exit }')
    [ "${mine%.w}" = "${ref%.w}" ] && [ -n "$mine" ] ||
        { echo "FAIL $tag: the instruction at 0x$pc is '$mine' in the report, '$ref' by llvm-objdump"; fail=1; }
}

f0=$fail
build m3-1 m3 1 -g && run m3-1 --board lm3s6965evb
[ $st = 0 ] && grep -q '^bus fault handled$' "$out/m3-1.o1" || { echo "FAIL m3-1: the handler did not run (status $st)"; fail=1; }
has m3-1 "embsim: fault: BusFault (exception 5) at 0x"
has m3-1 ", in crash (fault.c:$LOAD)"
has m3-1 "PRECISERR: a precise data bus error at 0x30000000"
has m3-1 "BFAR  0x30000000 (BFARVALID)"
has m3-1 "    r0  0x30000000"
has m3-1 "    lr: level2+0x"
bt m3-1 "crash:$LOAD" "level2:$CALL2" "level1:$CALL1" "main:$CALL0"
mnem m3-1 thumbv7m
if [ $have_od = 1 ]; then
    bf=$("$NM" "$out/m3-1.elf" | awk '$3 == "bus_fault" { print $1 }')
    has m3-1 "handler: bus_fault at 0x$bf"
fi

build m3-2 m3 2 -g && run m3-2 --board lm3s6965evb
[ $st = 3 ] || { echo "FAIL m3-2: status $st, not the lockup's 3"; fail=1; }
has m3-2 "embsim: fault: HardFault (exception 3) at 0x"
has m3-2 "FORCED: a configurable fault escalated to HardFault"
has m3-2 "DIVBYZERO: a divide by zero (CCR.DIV_0_TRP)"
has m3-2 "handler: none -- the vector is 0x00000000, without the Thumb bit"
has m3-2 "embsim: the core locked up at 0x00000000, in 0x00000000: the fault above had no handler"
bt m3-2 "crash:$DIV" "level2:$CALL2" "level1:$CALL1" "main:$CALL0"
mnem m3-2 thumbv7m

build m3-3 m3 3 -g && run m3-3 --board lm3s6965evb
has m3-3 "PRECISERR: a precise data bus error at 0x30000000"
has m3-3 "<- an exception's entry (EXC_RETURN 0xfffffff9): the code it interrupted"
got=$(awk '/^    #[0-9]/ { f = $4; sub(/\+0x[0-9a-f]*$/, "", f); printf "%s ", f }' "$out/m3-3.rep")
case $got in
"crash level2 tick spin main "*) ;;
*) echo "FAIL m3-3: the backtrace through the exception is '$got'"; fail=1 ;;
esac

build m3-nog m3 1 && run m3-nog --board lm3s6965evb
has m3-nog "backtrace (the calls EmbSim saw the core make):"
got=$(awk '/^    #[0-9]/ { f = $4; sub(/\+0x[0-9a-f]*$/, "", f); printf "%s ", f }' "$out/m3-nog.rep")
case $got in
"crash level2 level1 main "*) ;;
*) echo "FAIL m3-nog: the calls EmbSim saw are '$got'"; fail=1 ;;
esac
[ $fail = "$f0" ] && echo "  M3: a BusFault handled, a HardFault forced from a divide by zero, one in an interrupt handler, and one without -g: decoded, disassembled, unwound"

f0=$fail
build rv32-1 rv32 1 -g && run rv32-1 --board virt --ram-size 8M
[ $st = 3 ] || { echo "FAIL rv32-1: status $st, not the lockup's 3"; fail=1; }
has rv32-1 "embsim: fault: load access fault (mcause 5) at 0x"
has rv32-1 ", in crash (fault.c:$LOAD)"
has rv32-1 "mtval 0x30000000: the address loaded from"
has rv32-1 " a0   0x30000000"
has rv32-1 "handler: none -- mtvec is 0x00000000, where there is nothing to fetch: the core locks up"
has rv32-1 "the fault above had no handler"
bt rv32-1 "crash:$LOAD" "level2:$CALL2" "level1:$CALL1" "main:$CALL0"
mnem rv32-1 riscv32

build rv32-2 rv32 2 -g && run rv32-2 --board virt --ram-size 8M
[ $st = 0 ] && grep -q '^illegal instruction handled$' "$out/rv32-2.o1" ||
    { echo "FAIL rv32-2: the handler did not run (status $st)"; fail=1; }
has rv32-2 "embsim: fault: illegal instruction (mcause 2) at 0x"
has rv32-2 "mtval 0x7ff01073: the instruction's bits"
has rv32-2 ": 7ff01073  csrrw zero, 0x7ff, zero"
has rv32-2 "handler: mtvec 0x"
has rv32-2 ", trap"
bt rv32-2 "crash:$CSR" "level2:$CALL2" "level1:$CALL1" "main:$CALL0"
[ $fail = "$f0" ] && echo "  RV32: a load access fault with no handler, an illegal instruction handled: decoded, disassembled, unwound"

# exc.c: two faults, both handled, both reported; the run unchanged
x=exc
if "$EMBCC" --target=thumbv7m-none-eabi -O2 -c tests/golden/embsim/exc.c -o "$out/exc.o" &&
   sh tools/build-rt.sh thumbv7m-none-eabi "$out/rt" > "$out/rt.log" 2>&1 &&
   "$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/exc.o" "$out/rt/librt.a" -o "$out/exc.elf" > /dev/null 2>&1; then
    run exc --board lm3s6965evb
    n=$(grep -c '^embsim: fault: ' "$out/exc.rep")
    [ "$n" = 2 ] && grep -q '^embsim: fault: UsageFault (exception 6)' "$out/exc.rep" &&
        grep -q '^embsim: fault: HardFault (exception 3)' "$out/exc.rep" ||
        { echo "FAIL exc: $n reports, not a UsageFault's and a HardFault's"; fail=1; }
else
    echo "FAIL exc: exc.c does not build"; fail=1
fi

# the AVR: an instruction the part lacks locks it up; no fault to decode
a=$out/avr; mkdir -p "$a"
printf '\t.text\n\t.globl bad\nbad:\n\telpm\n\tret\n' > "$a/bad.S"
printf 'void bad(void);\nvolatile int sink;\n__attribute__((noinline)) void outer(void) { bad(); sink = 1; }\nint main(void) { outer(); return 0; }\n' > "$a/lock.c"
if "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$a/boot.o" &&
   "$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$a/io.o" &&
   "$EMBCC" --target=avr -c "$a/bad.S" -o "$a/bad.o" &&
   "$EMBCC" --target=avr -O1 -c "$a/lock.c" -o "$a/lock.o" &&
   EMBCC_AVR_HARNESS=$a sh tests/harness/avr/link.sh "$out/avr.elf" "$a/lock.o" "$a/bad.o" > /dev/null 2>&1; then
    run avr --board uno
    has avr "embsim: the core locked up at 0x"
    has avr ", in bad"
    got=$(awk '/^    #[0-9]/ { f = $4; sub(/\+0x[0-9a-f]*$/, "", f); printf "%s ", f }' "$out/avr.rep")
    case $got in
    "bad outer main "*) ;;
    *) echo "FAIL avr: the calls at the lockup are '$got'"; fail=1 ;;
    esac
else
    echo "FAIL avr: the lockup image does not build"; fail=1
fi
[ $fail = 0 ] && echo "embsim --fault-report: Cortex-M and RISC-V faults decoded and unwound, handled or not; the run unchanged"
exit $fail
