#!/bin/sh
# Xtensa assembly: the vocabulary against QEMU's de212 disassembler and
# GNU as, a file's layout against GNU as, programs that use it on the
# board, and what is refused.
#
#  1. tools/xtasmcheck assembles every entry of src/arch/xtensa/asm.c's
#     vocabulary -- generated from its own tables, laid out at known
#     addresses -- and QEMU's de212 core, stopped before it runs, has its
#     monitor disassemble the bytes (`xp/Ni`), as xtensa-encoding.sh does
#     for the encoder. Each line must read back as the statement means it:
#     the operands in their places, a special register by its name, a
#     branch's, call's, loop's or l32r's absolute target. The few forms
#     the de212 lacks (the ESP32's CPENABLE, BR, MISC2/3, THREADPTR) are
#     marked; Espressif's GNU as for the ESP32, where it is installed,
#     assembles EVERY line -- the same statements, so GNU's syntax is the
#     spec -- and its bytes must equal EmbCC's.
#  2. layout.S, assembled by EmbCC and by GNU as (--text-section-literals),
#     linked by embld at the same address: the images must be identical --
#     where .literal_position, .literal and a function's entry put the
#     pools, .align in bytes, labels, local and external calls and
#     branches, data words.
#  3. tests/golden/xtensa-asm/main.c (inline asm of every operand kind,
#     PS/CCOUNT/EXCSAVE1, a callx8 from a template, values live across
#     clobbering templates; a file-scope asm function, a naked function)
#     linked with forms.S (calls both ways with call4/call8/call12/callx8,
#     literal pools, numeric labels, the zero-overhead loop, the branch
#     forms, rsil/rsr/wsr/xsr/rsync) runs on QEMU's de212 at -O0, -O1, -O2
#     and -Os -- forms.S assembled by EmbCC and, for the reference, by GNU.
#  4. An instruction outside the vocabulary, an operand out of range, a
#     density form, an inline template's call0, a0/a1 clobbers, a symbol
#     in a template, and the directives that would change code are each
#     refused by name.
set -u
echo "TEST-MARKER xtensa-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_XTENSA:-qemu-system-xtensa}
T=xtensa-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/xtensa-asm
out=tests/golden/out/xtensa-asm
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1
# Espressif's binutils, beside the reference GCC (tools/xtensa-ref-gcc.sh)
GAS=$(dirname "$XTENSA_REF_GCC")/xtensa-esp32-elf-as
GOBJCOPY=$(dirname "$XTENSA_REF_GCC")/xtensa-esp-elf-objcopy
[ -x "$GAS" ] && [ -x "$GOBJCOPY" ] || GAS=

cc -std=c99 -Wall -Wextra -o "$out/xtasmcheck" \
   tools/xtasmcheck/xtasmcheck.c src/arch/xtensa/asm.c \
   src/arch/xtensa/emit.c src/arch/code.c src/arch/target.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
   src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "xtasmcheck did not build"; exit 1; }

# ---- 1. the vocabulary -------------------------------------------------------
BASE=0x60100000
ORG=0x80000           # far enough in that a call or l32r may reach back
"$out/xtasmcheck" --vocab $BASE $ORG > "$out/v.txt" 2> "$out/v.err" || {
    echo "the assembler refused its own vocabulary:"; head -3 "$out/v.err"
    exit 1; }
n=$(wc -l < "$out/v.txt" | tr -d ' ')
[ "$n" -ge 1000 ] || { echo "the vocabulary is only $n statements"; exit 1; }
cut -d'|' -f1 "$out/v.txt" > "$out/want.txt"
cut -d'|' -f2 "$out/v.txt" > "$out/ours.hex"
cut -d'|' -f3- "$out/v.txt" > "$out/stmts.txt"
tr -d '\n' < "$out/ours.hex" | perl -ne 'print pack("H*", $_)' > "$out/blob.bin"
if command -v "$QEMU" >/dev/null 2>&1; then
    printf 'xp/%di 0x%x\nquit\n' "$n" $((BASE + ORG)) |
        "$QEMU" -M sim -cpu de212 -S -display none -monitor stdio \
            -device loader,file="$out/blob.bin",addr=$((BASE + ORG)) \
            > "$out/qemu.raw" 2>&1
    tr -d '\r' < "$out/qemu.raw" | sed -n 's/^0x[0-9a-f]*:  //p' > "$out/got.txt"
    m=$(wc -l < "$out/got.txt" | tr -d ' ')
    [ "$m" = "$n" ] || {
        echo "QEMU disassembled $m lines of the $n"; head -5 "$out/qemu.raw"
        exit 1; }
    paste -d'|' "$out/want.txt" "$out/got.txt" "$out/stmts.txt" |
        awk -F'|' 'substr($1, 1, 1) != "!" && $1 != $2 {
                       print "  " $3 ":  want " $1 "   QEMU " $2 }' \
        > "$out/diff.txt"
    if [ -s "$out/diff.txt" ]; then
        echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n statements decode to something else:"
        head -20 "$out/diff.txt"; exit 1
    fi
    nq=$(grep -vc '^!' "$out/want.txt")
    echo "$nq Xtensa asm statements decode on QEMU's de212 as they are written"
else
    echo "SKIP the QEMU half: $QEMU not found"
fi
if [ -n "$GAS" ]; then
    { printf '\t.text\n\t.space %d\n' $((ORG)); sed 's/^/\t/' "$out/stmts.txt"; } \
        > "$out/g.s"
    "$GAS" --no-transform "$out/g.s" -o "$out/g.o" 2> "$out/g.err" || {
        echo "GNU as rejected the vocabulary -- an entry claims a form it"
        echo "does not have:"; grep -i error "$out/g.err" | head -5; exit 1; }
    "$GOBJCOPY" -O binary -j .text "$out/g.o" "$out/g.bin"
    tail -c +$((ORG + 1)) "$out/g.bin" | od -An -v -tx1 | tr -d ' \n' |
        fold -w6 > "$out/g.hex"
    echo >> "$out/g.hex"
    paste -d'|' "$out/ours.hex" "$out/g.hex" "$out/stmts.txt" |
        awk -F'|' '$1 != $2 { print "  " $3 ":  ours " $1 ", GNU " $2 }' \
        > "$out/gdiff.txt"
    if [ -s "$out/gdiff.txt" ]; then
        echo "$(wc -l < "$out/gdiff.txt" | tr -d ' ') statements encode differently from GNU as:"
        head -20 "$out/gdiff.txt"; exit 1
    fi
    echo "all $n encode as Espressif's GNU as encodes them"
else
    echo "SKIP the GNU half: no xtensa-esp32-elf-as"
fi
# a numeric label in a template is the distance it names
printf '1: addi a2, a2, -1|bnez a2, 1b\naddi a2, a2, -1|bnez a2, .-3\nbeqz a3, 2f|nop|nop|2: ret\nbeqz a3, .+9|nop|nop|ret\nloop a4, 1f|addi a5, a5, 1|1: nop\nloop a4, .+6|addi a5, a5, 1|nop\n' |
    "$out/xtasmcheck" --bytes > "$out/lab.txt" 2> "$out/lab.err" || {
    echo "a template with labels was refused:"; cat "$out/lab.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/lab.txt" || {
    echo "a numeric label does not encode as its distance:"; cat "$out/lab.txt"
    exit 1; }

# ---- 2. a file's layout, against GNU as ------------------------------------------
cat > "$out/stubs.c" <<'EOF'
int ext_data[4] = { 1, 2, 3, 4 };
void ext_fn(void) { }
void ext_tail(void) { }
EOF
"$EMBCC" --target=$T -O1 -c "$out/stubs.c" -o "$out/stubs.o" &&
"$EMBCC" --target=$T -c "$d/layout.S" -o "$out/layout.o" || {
    echo "layout.S does not assemble"; exit 1; }
if [ -n "$GAS" ]; then
    "$XTENSA_REF_GCC" $XTENSA_REF_FLAGS -mtext-section-literals -c \
        "$d/layout.S" -o "$out/layout-gnu.o" 2> "$out/layout-gnu.err" || {
        echo "GNU as: layout.S"; cat "$out/layout-gnu.err"; exit 1; }
    for a in layout layout-gnu; do
        "$EMBLD" -e lay_a -Ttext 0x60010000 "$out/$a.o" "$out/stubs.o" \
            -o "$out/$a.elf" > "$out/$a.lerr" 2>&1 || {
            echo "embld could not link $a.o:"; head -3 "$out/$a.lerr"; exit 1; }
        "$GOBJCOPY" -O binary "$out/$a.elf" "$out/$a.bin"
        ${READELF:-llvm-readelf} -s "$out/$a.elf" |
            awk '$8 ~ /^(lay_|vec|ext_)/ { print $2, $8 }' | sort -k2 > "$out/$a.sym"
    done
    cmp -s "$out/layout.bin" "$out/layout-gnu.bin" || {
        echo "layout.S links to a different image from GNU as's:"
        cmp "$out/layout.bin" "$out/layout-gnu.bin" | head -3; exit 1; }
    cmp -s "$out/layout.sym" "$out/layout-gnu.sym" &&
        [ "$(wc -l < "$out/layout.sym" | tr -d ' ')" -ge 5 ] || {
        echo "layout.S's symbols are not where GNU as puts them:"
        diff "$out/layout.sym" "$out/layout-gnu.sym" | head; exit 1; }
    echo "layout.S links to the image GNU as's object links to: pools, alignment, labels, relocations"
fi

# ---- 3. on the board ---------------------------------------------------------------
if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_XTENSA_HARNESS="$PWD/$out"
    "$EMBCC" --target=$T -O1 -c tests/harness/xtensa/boot.c \
        -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/xtensa/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    want1="82 77 5 976 -300 72 83 1502 55 40 1 194 155 550 523 523 99 775"
    want2="82 43 110 59 80 1 775 127 1040 327685 1 1234 43 100041 42"
    asms=embcc
    [ -n "$GAS" ] && asms="embcc gnu"
    for as in $asms; do
        if [ $as = gnu ]; then
            "$XTENSA_REF_GCC" $XTENSA_REF_FLAGS -mtext-section-literals -c \
                "$d/forms.S" -o "$out/forms-$as.o" || { echo "GNU as: forms.S"; exit 1; }
        else
            "$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms-$as.o" || {
                echo "EmbCC could not assemble forms.S"; exit 1; }
        fi
        for opt in -O0 -O1 -O2 -Os; do
            tag=$as$opt
            [ -f "$out/main$opt.o" ] ||
            "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
                echo "main.c $opt does not compile"; exit 1; }
            EMBLD="$EMBLD" sh tests/harness/xtensa/link.sh "$out/$tag.elf" \
                "$out/main$opt.o" "$out/forms-$as.o" > "$out/$tag.lerr" 2>&1 || {
                echo "$tag: embld could not link it:"; head -3 "$out/$tag.lerr"
                exit 1; }
            sh tests/harness/xtensa/run.sh "$out/$tag.elf" > "$out/$tag.txt"
            got1=$(sed -n '1p' "$out/$tag.txt" | sed 's/ *$//')
            got2=$(sed -n '2p' "$out/$tag.txt" | sed 's/ *$//')
            [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] || {
                echo "$tag: the program printed"
                head -3 "$out/$tag.txt" | sed 's/^/    /'
                echo "  not"; echo "    $want1"; echo "    $want2"; exit 1; }
        done
    done
    echo "inline asm, a file-scope block, a naked function and forms.S"
    echo "(assembled by: $asms) run on the de212 at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 4. the refusals ---------------------------------------------------------------
"$out/xtasmcheck" --refuse > "$out/ref.txt" <<'REF'
add.s f0, f1, f2
mov.n a2, a3
ret.n
addi a2, a3, 128
addi a2, a3, -129
addmi a2, a3, 300
movi a2, 2048
slli a2, a3, 0
srli a2, a3, 16
extui a2, a3, 0, 17
l32i a2, a3, 1022
l16si a2, a3, 1
l32e a2, a3, 0
beq a2, a3, .+132
beqz a2, .+2052
beqi a2, 9, .+8
bltui a2, -1, .+8
bbci a2, 32, .+8
j .+131076
loop a2, .+3
call8 .+8
l32r a2, .-8
entry a1, 36
rsil a2, 16
rotw 8
rsr a2, intset
wsr a2, prid
xsr a2, interrupt
rsr a2, gpio_out
rsr a2, 256
add a2, a3, a16
beqz a2, 8
foo: nop
1: beqz a2, 2b
ssa8b a2
REF
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the Xtensa vocabulary"*|*"density option"*|*" is not "*|\
    *"does not fit"*|*"out of reach"*|*"cannot test"*|*"inline asm does not know"*|\
    *"cannot be read"*|*"cannot be written"*|*"cannot be exchanged"*|\
    *"is not a register"*|*"is not a branch target"*|*"are numeric"*|\
    *"refers to no label"*|*"not 4-aligned"*|*"the shift is 0..31"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 35 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "a call0 in a template" "callx0 in Xtensa asm writes a0" \
    'void f(void){ __asm__ volatile("callx0 a8"); }'
refc "a0 clobbered" "clobbers 'a0', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "a0"); }'
refc "sp clobbered" "clobbers 'a1', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "a1"); }'
refc "an unknown clobber" "is not an Xtensa register" \
    'void f(void){ __asm__ volatile("nop" ::: "eax"); }'
refc "a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("call8 g"); }'
refc "a named label in a template" "are numeric" \
    'void f(void){ __asm__ volatile("x: j x"); }'
refc "x86's constraint letters" 'is not valid for Xtensa' \
    'int f(int x){ int r; __asm__("mov %0, %1" : "=r"(r) : "b"(x)); return r; }'
refc "a 64-bit operand" 'is 8 bytes' \
    'long long f(long long x){ __asm__("nop" :: "r"(x)); return x; }'
refc "a register variable in a7" "is not supported for Xtensa asm" \
    'int f(void){ register int r __asm__("a7") = 1; __asm__("nop" :: "r"(r)); return r; }'
refc "a modifier" "modifier '%x' is not supported" \
    'int f(int x){ __asm__("nop # %x0" :: "r"(x)); return x; }'
refc "a wide movi in a template" "does not fit its signed 12 bits" \
    'int f(void){ int r; __asm__("movi %0, 5000" : "=r"(r)); return r; }'
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a long-call block" "longcalls is not supported" '	.begin longcalls'
refs "_movi of a symbol" "_movi takes a constant" '	_movi a2, foo'
refs "a symbol where none relocates" "cannot relocate" '	addi a2, a3, foo'
refs ".align 3" "a power of two" '	.align 3'
refs "an unaligned call target" "not 4-aligned" '	call8 1f
1:	nop'
refs "a literal out of reach" "out of reach" '	.literal_position
	.literal .Lx, 1
	.space 300000
	l32r a2, .Lx'
echo "$nr statements, eleven inline templates and six files are each refused by name"
