#!/bin/sh
# Division by a constant and the widening multiply on the 32-bit targets,
# by the instructions they become.
#
# A 32-bit machine with a widening multiply has the high word of a 32x32
# product in one instruction, so `x / 7` at -O2 is a multiply by the
# divisor's magic number and shifts (pass_divmagic, IR mulh) -- not udiv
# (two to twelve cycles on a Cortex-M), not divu (dozens on most RISC-V
# cores), not a call to __aeabi_uidiv (ARM state). And `(int64_t)a * b`
# of two 32-bit values is ONE widening multiply (pass_mulwiden, IR mulw),
# where it was a 64x64 multiply: four multiplies on RV32, three on Thumb.
# tests/exec/divconst-32.c and mulwide-32.c check what they compute; this
# checks that they are what runs.
#
# Every target with the instruction (target_has_mulh): RV32, ARMv7-M,
# ARMv7E-M, ARM state, MIPS32 in both byte orders, PowerPC, SPARC V8,
# TriCore and RX. TriCore and RX have no disassembler here (LLVM has
# neither target), so theirs are read as encodings.
set -u
echo "TEST-MARKER divmul32-isel"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/divmul32-isel
rm -rf "$out"; mkdir -p "$out"

cat > "$out/d.c" <<'EOF'
unsigned d7(unsigned x) { return x / 7; }
unsigned m10(unsigned x) { return x % 10; }
int s5(int x) { return x / 5; }
long long smul(int a, int b) { return (long long)a * b; }
unsigned long long umul(unsigned a, unsigned b) { return (unsigned long long)a * b; }
long long sacc(long long s, int a, int b) { return s + (long long)a * b; }
EOF
fail=0
bad() { echo "FAIL: $*"; fail=1; }

# the IR first: the operations themselves, and only where they belong
"$EMBCC" inspect ir -O2 --target=riscv32-unknown-elf "$out/d.c" > "$out/rv.ir" ||
    bad "rv32 IR does not build"
fn_ir() { awk -v f="func @$2 " 'index($0, f) == 1 { on = 1 } on { print } /^}/ { on = 0 }' "$1"; }
fn_ir "$out/rv.ir" d7 | grep -q ' = mulh\.4 ' || bad "rv32: x / 7 is not a mulh in the IR"
fn_ir "$out/rv.ir" s5 | grep -q ' = mulh\.4s ' || bad "rv32: x / 5 is not a signed mulh"
fn_ir "$out/rv.ir" smul | grep -q ' = mulw\.8s ' || bad "rv32: (int64)a * b is not a signed mulw"
fn_ir "$out/rv.ir" umul | grep -q ' = mulw\.8 ' || bad "rv32: the unsigned product is not an unsigned mulw"
"$EMBCC" inspect ir -O2 --target=x86_64-elf "$out/d.c" > "$out/x86.ir" ||
    bad "x86-64 IR does not build"
grep -q 'mulh\|mulw' "$out/x86.ir" && bad "x86-64 has a 64-bit multiply and must not get mulh/mulw"
"$EMBCC" inspect ir -Os --target=riscv32-unknown-elf "$out/d.c" > "$out/rv-Os.ir" ||
    bad "rv32 -Os IR does not build"
fn_ir "$out/rv-Os.ir" d7 | grep -q ' = div\.4 ' || bad "rv32 -Os: the divide instruction is the shorter, and must stay"

# count TAG FUNC REGEX: matching lines of FUNC in $out/TAG.dis
count() {
    awk -v f="<$2>:" -v re="$3" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                                 on && $0 ~ re { n++ } END { print n + 0 }' "$out/$1.dis"
}
want() {                        # want TAG FUNC REGEX N WHAT
    got=$(count "$1" "$2" "$3")
    [ "$got" = "$4" ] || bad "$1 $2: $5 (counted $got of /$3/, want $4)"
}
dis() {                         # dis TAG TRIPLE ODTRIPLE [-O]
    "$EMBCC" --target=$2 ${4:--O2} -c "$out/d.c" -o "$out/$1.o" 2> "$out/$1.err" || {
        bad "$1: does not compile: $(head -1 "$out/$1.err")"; return 1; }
    "$OD" -d --no-show-raw-insn --triple=$3 "$out/$1.o" > "$out/$1.dis"
}

if dis rv32 riscv32-unknown-elf riscv32; then
    want rv32 d7 '\tmulhu\t' 1 "x / 7 should be one mulhu"
    want rv32 d7 '\tdivu\t' 0 "x / 7 should not divide"
    want rv32 m10 '\tmulhu\t' 1 "x % 10 should be one mulhu"
    want rv32 m10 '\tremu\t' 0 "x % 10 should not divide"
    want rv32 s5 '\tmulh\t' 1 "x / 5 should be one mulh"
    want rv32 s5 '\tdiv\t' 0 "x / 5 should not divide"
    want rv32 smul '\tmul' 2 "(int64)a * b should be mulh and mul"
    want rv32 smul '\tmulh\t' 1 "(int64)a * b should take a SIGNED high word"
    want rv32 umul '\tmulhu\t' 1 "the unsigned product should take mulhu"
    want rv32 umul '\tmul' 2 "the unsigned product should be two multiplies"
fi
for tag in m4 m3 a7; do
    case $tag in
        m4) T=thumbv7em-none-eabi; OT=thumbv7em ;;
        m3) T=thumbv7m-none-eabi; OT=thumbv7m ;;
        a7) T=armv7a-none-eabi; OT=armv7a ;;
    esac
    dis $tag $T $OT || continue
    want $tag d7 '\tumull\t' 1 "x / 7 should be one umull"
    want $tag d7 'div\t|\tbl' 0 "x / 7 should neither divide nor call"
    want $tag m10 '\tumull\t' 1 "x % 10 should be one umull"
    want $tag m10 '\tmls\t' 1 "x % 10 should take its remainder with mls"
    want $tag m10 'div\t|\tbl' 0 "x % 10 should neither divide nor call"
    if [ $tag = a7 ]; then
        want $tag s5 '\tsmmul\t' 1 "x / 5 should be one smmul"
    else
        # v7-M and v7E-M select alike, and smmul is v7E-M's DSP set
        want $tag s5 '\tsmull\t' 1 "x / 5 should be smull (no smmul on the M profile)"
    fi
    want $tag s5 'div\t|\tbl' 0 "x / 5 should neither divide nor call"
    want $tag smul 'mul' 1 "(int64)a * b should be one multiply"
    want $tag smul '\tsmull\t' 1 "(int64)a * b should be smull"
    want $tag umul '\tumull\t' 1 "the unsigned product should be umull"
    want $tag umul 'mul' 1 "the unsigned product should be one multiply"
    want $tag sacc 'mul|mla|adc' 1 "s + (int64)a * b should be one smlal"
    want $tag sacc '\tsmlal\t' 1 "s + (int64)a * b should be smlal"
done
# ARM state divides by calling __aeabi_uidiv: the multiply at -Os too
if dis a7s armv7a-none-eabi armv7a -Os; then
    want a7s d7 '\tumull\t' 1 "-Os: x / 7 should still be umull, not a call"
    want a7s d7 '\tbl' 0 "-Os: x / 7 should not call __aeabi_uidiv"
fi
for tag in mipsel mipseb; do
    case $tag in
        mipsel) T=mipsel-none-elf; OT=mipsel ;;
        mipseb) T=mips-none-elf; OT=mips ;;
    esac
    dis $tag $T $OT || continue
    want $tag d7 '\tmultu\t' 1 "x / 7 should be one multu"
    want $tag d7 '\tmfhi\t' 1 "x / 7 should read HI"
    want $tag d7 'div' 0 "x / 7 should not divide"
    want $tag s5 '\tmult\t' 1 "x / 5 should be one mult"
    want $tag s5 'div' 0 "x / 5 should not divide"
    want $tag smul '\tmult\t' 1 "(int64)a * b should be one mult"
    want $tag smul 'mul' 1 "(int64)a * b should be one multiply"
    want $tag umul '\tmultu\t' 1 "the unsigned product should be one multu"
done
if dis ppc powerpc-none-eabi powerpc; then
    want ppc d7 '\tmulhwu' 1 "x / 7 should be one mulhwu"
    want ppc d7 'divw' 0 "x / 7 should not divide"
    want ppc s5 '\tmulhw ' 1 "x / 5 should be one mulhw"
    want ppc s5 'divw' 0 "x / 5 should not divide"
    want ppc smul 'mul' 2 "(int64)a * b should be mulhw and mullw"
    want ppc smul '\tmulhw ' 1 "(int64)a * b should take a SIGNED high word"
    want ppc umul '\tmulhwu' 1 "the unsigned product should take mulhwu"
fi
if dis sparc sparc-none-elf sparc; then
    want sparc d7 '\tumul ' 1 "x / 7 should be one umul"
    want sparc d7 '%y' 1 "x / 7 should read %y"
    want sparc d7 'div' 0 "x / 7 should not divide"
    want sparc s5 '\tsmul ' 1 "x / 5 should be one smul"
    want sparc s5 'div' 0 "x / 5 should not divide"
    want sparc smul 'mul' 1 "(int64)a * b should be one multiply"
    want sparc smul '\tsmul ' 1 "(int64)a * b should be smul"
    want sparc umul '\tumul ' 1 "the unsigned product should be umul"
fi

# TriCore, from the assembly's bytes: RR2 opcode 0x73 with op2 0x68 is
# MUL.U into an E register, 0x6a MUL, 0x0a the 32-bit MUL; RR 0x4b with
# op2 0x20/0x21 is DIV/DIV.U.
"$EMBCC" --target=tricore-none-elf -O2 -S "$out/d.c" -o "$out/tc.s" 2> "$out/tc.err" ||
    bad "tricore: does not compile: $(head -1 "$out/tc.err")"
tcfn() { awk -v f="$1:" '$1 == f { on = 1; next } /^$/ { on = 0 } on' "$out/tc.s"; }
tcn() { tcfn "$1" | grep -c "$2"; }
[ "$(tcn d7 'byte	0x73,0x..,0x68,0x.0$')" = 1 ] || bad "tricore d7: x / 7 should be one MUL.U"
[ "$(tcn d7 'byte	0x4b,0x..,0x[01].,0x.2$')" = 0 ] || bad "tricore d7: x / 7 should not divide"
[ "$(tcn s5 'byte	0x73,0x..,0x6a,0x.0$')" = 1 ] || bad "tricore s5: x / 5 should be one MUL (signed, 64)"
[ "$(tcn s5 'byte	0x4b,0x..,0x[01].,0x.2$')" = 0 ] || bad "tricore s5: x / 5 should not divide"
[ "$(tcn smul 'byte	0x73,')" = 1 ] && [ "$(tcn smul 'byte	0x73,0x..,0x6a,0x.0$')" = 1 ] ||
    bad "tricore smul: (int64)a * b should be one signed MUL"
[ "$(tcn umul 'byte	0x73,')" = 1 ] && [ "$(tcn umul 'byte	0x73,0x..,0x68,0x.0$')" = 1 ] ||
    bad "tricore umul: the unsigned product should be one MUL.U"

# RX, from the object's bytes, one function an object: FC 1B/1F is
# emul/emulu of two registers, FC 23/27 div/divu.
for f in d7 s5 smul umul; do
    awk -v f="$f" 'index($0, f "(") > 0' "$out/d.c" > "$out/rx-$f.c"
    "$EMBCC" --target=rx-none-elf -O2 -c "$out/rx-$f.c" -o "$out/rx-$f.o" 2> "$out/rx.err" || {
        bad "rx $f: does not compile: $(head -1 "$out/rx.err")"; continue; }
    llvm-objcopy -O binary --only-section=.text "$out/rx-$f.o" "$out/rx-$f.bin"
    od -An -tx1 -v "$out/rx-$f.bin" | tr -d ' \n' > "$out/rx-$f.hex"
done
rxhas() { grep -c "^\(..\)*$2" "$out/rx-$1.hex"; }
[ "$(rxhas d7 fc1f)" = 1 ] && [ "$(rxhas d7 fc27)" = 0 ] ||
    bad "rx d7: x / 7 should be emulu and no divu: $(cat "$out/rx-d7.hex")"
[ "$(rxhas s5 fc1b)" = 1 ] && [ "$(rxhas s5 fc23)" = 0 ] ||
    bad "rx s5: x / 5 should be emul and no div: $(cat "$out/rx-s5.hex")"
[ "$(rxhas smul fc1b)" = 1 ] || bad "rx smul: (int64)a * b should be emul: $(cat "$out/rx-smul.hex")"
[ "$(rxhas umul fc1f)" = 1 ] || bad "rx umul: the unsigned product should be emulu: $(cat "$out/rx-umul.hex")"

[ $fail = 0 ] || exit 1
echo "x / 7, x % 10, x / 5 are a high multiply and (int64)a * b one widening"
echo "multiply on RV32, ARMv7-M, ARMv7E-M, ARM state, MIPS32 (both orders),"
echo "PowerPC, SPARC, TriCore and RX; x86-64 keeps its 64-bit multiply"
