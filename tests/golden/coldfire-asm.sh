#!/bin/sh
# ColdFire assembly: the vocabulary against QEMU's m68k disassembler, the
# file assembler against the statement assembler, FreeRTOS's ColdFire V2
# port, programs that use it on the board against a host model, and what
# is refused.
#
# There is no GNU or LLVM m68k assembler here, so QEMU is the referee, as
# it is for the encoder (coldfire-encoding.sh):
#  1. tools/cfasmcheck assembles every entry of src/arch/coldfire/asm.c's
#     vocabulary -- generated from its own samples: each size and legal
#     pair of effective-address kinds of move, every register in every
#     field, the ALU's five forms, the immediates at their ends, movem,
#     every condition, branches at the ends of their reach with and without
#     a size, the moves to and from %sr/%ccr/%usp, movec, the bit
#     instructions -- written in every spelling the parser takes (Motorola
#     with and without %, either case, d(An) and (d,An), MIT's An@(d)),
#     laid out at a known address. QEMU's mcf5208evb, stopped before it
#     runs, has its monitor disassemble the bytes (`xp/Ni`); each line must
#     read back as the statement means it -- binutils' MIT text, with a
#     branch's or a %pc operand's absolute target.
#  2. The same statements in a .s file, assembled by `embcc -c` (src/as/
#     gas.c), must be the same bytes; and FreeRTOS's portable/GCC/
#     ColdFire_V2/portasm.S, where it is in ~/EmbRef, assembles and reads
#     back as its source says.
#  3. tests/golden/coldfire-asm/main.c (inline asm of every operand kind,
#     %sr/%ccr/%usp, a trap through a vector installed with movec %vbr,
#     numeric labels and a loop, values live across templates that clobber
#     named and unnamed registers, a call from a template, divide and
#     remainder, a naked function, a file-scope block) linked with forms.S
#     (C calling assembly and assembly calling C, a tail call, a jump
#     table, relaxed branches over 20 KB, the bit, divide and frame
#     instructions, an exception return) runs on the board at -O0, -O1,
#     -O2 and -Os; what it prints must be what tests/golden/coldfire-asm/
#     model.c, compiled for the host, computes from the C meaning of each.
#  4. Instructions ColdFire lacks, operands out of range, a symbol in a
#     template, a frame or stack pointer clobbered, and a .s file's forms
#     that do not relocate are each refused by name.
set -u
echo "TEST-MARKER coldfire-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_M68K:-qemu-system-m68k}
T=m68k-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/coldfire-asm
out=tests/golden/out/coldfire-asm
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1
OBJCOPY=${EMBCC_OBJCOPY:-llvm-objcopy}
READELF=${EMBCC_LLVM_READELF:-llvm-readelf}

cc -std=c99 -Wall -Wextra -o "$out/cfasmcheck" \
   tools/cfasmcheck/cfasmcheck.c src/arch/coldfire/asm.c \
   src/arch/coldfire/emit.c src/arch/code.c src/arch/target.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
   src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "cfasmcheck did not build"; exit 1; }

# QEMU's disassembly of a blob loaded at 0x40000000: qdis BLOB N OUT
qdis() {
    printf 'xp/%di 0x40000000\nquit\n' "$2" > "$out/cmds.txt"
    TMPDIR=${TMPDIR:-/tmp} sh tests/harness/qrun.sh 120 "$QEMU" \
        -M mcf5208evb -cpu m5208 -S -display none -serial null \
        -chardev file,id=mon,path="$out/qemu.raw",input-path="$out/cmds.txt" \
        -mon chardev=mon -kernel "$1" 2>"$out/qemu.err"
    tr -d '\r' < "$out/qemu.raw" | sed -n 's/^0x[0-9a-f]*:  //p' > "$3"
}

# ---- 1. the vocabulary -------------------------------------------------------
BASE=0x40000000
"$out/cfasmcheck" --vocab $BASE > "$out/v.txt" 2> "$out/v.err" || {
    echo "the assembler refused its own vocabulary:"; head -3 "$out/v.err"
    exit 1; }
n=$(wc -l < "$out/v.txt" | tr -d ' ')
[ "$n" -ge 3500 ] || { echo "the vocabulary is only $n statements"; exit 1; }
cut -d'|' -f1 "$out/v.txt" > "$out/want.txt"
cut -d'|' -f2 "$out/v.txt" | tr -d '\n' | perl -ne 'print pack("H*", $_)' \
    > "$out/blob.bin"
cut -d'|' -f3- "$out/v.txt" > "$out/stmts.txt"
if command -v "$QEMU" >/dev/null 2>&1; then
    qdis "$out/blob.bin" "$n" "$out/got.txt"
    m=$(wc -l < "$out/got.txt" | tr -d ' ')
    [ "$m" = "$n" ] || {
        echo "QEMU disassembled $m lines of the $n -- a statement's length is"
        echo "not what the assembler thinks, or QEMU did not finish:"
        head -5 "$out/qemu.err"; exit 1; }
    paste -d'|' "$out/want.txt" "$out/got.txt" "$out/stmts.txt" |
        awk -F'|' '$1 != $2 { print "  " $3 ":  want " $1 "   QEMU " $2 }' \
        > "$out/diff.txt"
    if [ -s "$out/diff.txt" ]; then
        echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n statements decode to something else:"
        head -20 "$out/diff.txt"; exit 1
    fi
    echo "$n ColdFire asm statements decode on QEMU's m5208 as they are written"
else
    echo "SKIP the QEMU half: $QEMU not found"
fi
# a numeric label in a template is the distance it names
printf '1: subq.l #1,%%d0|bne 1b\nsubq.l #1,%%d0|bne .-2\nbeq 2f|nop|nop|2: rts\nbeq .+6|nop|nop|rts\nbra 3f|nop|3: rts\nbra .+4|nop|rts\n' |
    "$out/cfasmcheck" --bytes > "$out/lab.txt" 2> "$out/lab.err" || {
    echo "a template with labels was refused:"; cat "$out/lab.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/lab.txt" || {
    echo "a numeric label does not encode as its distance:"; cat "$out/lab.txt"
    exit 1; }

# ---- 2. the file assembler -------------------------------------------------------
{ printf '\t.text\n'; sed 's/^/\t/' "$out/stmts.txt"; } > "$out/v.s"
"$EMBCC" --target=$T -c "$out/v.s" -o "$out/v.o" 2> "$out/vs.err" || {
    echo "EmbCC's .s assembler refused the vocabulary:"; head -3 "$out/vs.err"
    exit 1; }
"$OBJCOPY" -O binary -j .text "$out/v.o" "$out/vs.bin"
cmp -s "$out/vs.bin" "$out/blob.bin" || {
    echo "the vocabulary as a .s file assembles to other bytes than"
    echo "statement by statement:"; cmp "$out/vs.bin" "$out/blob.bin" | head -2
    exit 1; }
echo "and the same $n statements in a .s file are the same bytes"
FRT=${EMBCC_FREERTOS:-$HOME/EmbRef/FreeRTOS-Kernel}/portable/GCC/ColdFire_V2/portasm.S
if [ -f "$FRT" ] && command -v "$QEMU" >/dev/null 2>&1; then
    "$EMBCC" --target=$T -c "$FRT" -o "$out/portasm.o" 2> "$out/portasm.err" || {
        echo "FreeRTOS's ColdFire V2 portasm.S does not assemble:"
        head -3 "$out/portasm.err"; exit 1; }
    "$OBJCOPY" -O binary -j .text "$out/portasm.o" "$out/portasm.bin"
    qdis "$out/portasm.bin" 35 "$out/portasm.txt"
    cat > "$out/portasm.want" <<'EOF'
linkw %fp,#-8
moveml %d6-%d7,%sp@
movew %sr,%d7
movel %d7,%d0
andil #1792,%d0
lsrl #8,%d0
movel %fp@(8),%d6
andil #7,%d6
lsll #8,%d6
andil #63743,%d7
orl %d6,%d7
movew %d7,%sr
moveml %sp@,%d6-%d7
lea %sp@(8),%sp
unlk %fp
rts
movel %sp@(4),%d0
movec %d0,%cacr
nop
rts
lea %sp@(-60),%sp
moveml %d0-%fp,%sp@
moveal 0x0,%a0
movel %sp,%a0@
jsr 0x0
moveal 0x0,%a0
moveal %a0@,%sp
moveml %sp@,%d0-%fp
lea %sp@(60),%sp
rte
moveal 0x0,%a0
moveal %a0@,%sp
moveml %sp@,%d0-%fp
lea %sp@(60),%sp
rte
EOF
    cmp -s "$out/portasm.txt" "$out/portasm.want" || {
        echo "FreeRTOS's portasm.S reads back as something else:"
        diff "$out/portasm.want" "$out/portasm.txt" | head; exit 1; }
    "$READELF" -r "$out/portasm.o" | grep -c 'R_68K_32 ' > "$out/portasm.nrel"
    [ "$(cat "$out/portasm.nrel")" = 4 ] || {
        echo "portasm.S has $(cat "$out/portasm.nrel") R_68K_32, not 4"; exit 1; }
    echo "FreeRTOS's ColdFire V2 portasm.S assembles and reads back as written"
else
    echo "SKIP FreeRTOS's portasm.S: no $FRT"
fi
# the relocations a .s file's symbol forms carry
printf '\t.text\n\tbsr ext_fn\n\tbra ext_fn\n\tbeq ext_fn\n\tbne.w ext_fn\n\tjsr ext_fn\n\tlea ext_data+8,%%a0\n\tmove.l #ext_data,%%d0\n\tmove.l ext_data,%%d1\n\tmove.l %%d1,ext_data+4\n\taddq.l #1,ext_data\n\tpea ext_fn\n\t.long ext_data\n\t.word ext_data\n' \
    > "$out/rel.s"
"$EMBCC" --target=$T -c "$out/rel.s" -o "$out/rel.o" || {
    echo "the symbol forms do not assemble"; exit 1; }
"$READELF" -r "$out/rel.o" | awk '/R_68K/ { print $1, $3, $5, $7 }' \
    > "$out/rel.txt"
cat > "$out/rel.want" <<'EOF'
00000002 R_68K_32 ext_fn 0
00000008 R_68K_32 ext_fn 0
0000000e R_68K_PC16 ext_fn 0
00000012 R_68K_PC16 ext_fn 0
00000016 R_68K_32 ext_fn 0
0000001c R_68K_32 ext_data 8
00000022 R_68K_32 ext_data 0
00000028 R_68K_32 ext_data 0
0000002e R_68K_32 ext_data 4
00000034 R_68K_32 ext_data 0
0000003a R_68K_32 ext_fn 0
0000003e R_68K_32 ext_data 0
00000042 R_68K_16 ext_data 0
EOF
cmp -s "$out/rel.txt" "$out/rel.want" || {
    echo "a symbol form carries other relocations:"
    diff "$out/rel.want" "$out/rel.txt"; exit 1; }

# ---- 3. on the board, against the host model -----------------------------------
if command -v "$QEMU" >/dev/null 2>&1; then
    cc -std=c99 -Wall -o "$out/model" "$d/model.c" &&
        "$out/model" > "$out/model.txt" || { echo "the model does not run"; exit 1; }
    want1=$(sed -n 1p "$out/model.txt")
    want2=$(sed -n 2p "$out/model.txt")
    export EMBCC_CF_HARNESS="$PWD/$out"
    "$EMBCC" --target=$T -O1 -c tests/harness/coldfire/boot.c -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/coldfire/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    "$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms.o" || {
        echo "EmbCC could not assemble forms.S"; exit 1; }
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
            echo "main.c $opt does not compile"; exit 1; }
        EMBLD="$EMBLD" sh tests/harness/coldfire/link.sh "$out/m$opt.elf" \
            "$out/main$opt.o" "$out/forms.o" > "$out/m$opt.lerr" 2>&1 || {
            echo "$opt: embld could not link it:"; head -3 "$out/m$opt.lerr"
            exit 1; }
        sh tests/harness/coldfire/run.sh "$out/m$opt.elf" > "$out/m$opt.txt"
        got1=$(sed -n '1p' "$out/m$opt.txt" | sed 's/ *$//')
        got2=$(sed -n '2p' "$out/m$opt.txt" | sed 's/ *$//')
        [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] &&
            grep -q '==EXIT 0 ==' "$out/m$opt.txt" || {
            echo "$opt: the program printed"
            head -4 "$out/m$opt.txt" | sed 's/^/    /'
            echo "  and the host model"; sed 's/^/    /' "$out/model.txt"; exit 1; }
    done
    echo "inline asm, a file-scope block, a naked function and forms.S run on"
    echo "the mcf5208evb at -O0, -O1, -O2 and -Os as the host model computes"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 4. the refusals ---------------------------------------------------------------
"$out/cfasmcheck" --refuse > "$out/ref.txt" <<'REF'
rol.l #1,%d0
roxr.l #1,%d0
dbra %d0,.-4
exg %d0,%d1
cas.l %d0,%d1,(%a0)
fmove.l %fp0,%d0
mvs.b %d1,%d0
frob %d0
add.b %d1,%d0
add.w %d1,%d0
sub.w #1,%d0
neg.w %d0
movea.b %d0,%a0
move.b %d0,%a0
move.l (8,%a0),(0x1000).l
move.l #1,(8,%a0)
move.l %d0,#1
move.l %d0,(4,%pc)
moveq #128,%d0
moveq #1,%a0
addq.l #0,%d0
addq.l #9,%d0
addi.l #1,%a0
add.l #1000,(%a0)
and.l %a0,%d0
eor.l (%a0),%d0
cmp.l %d0,(%a0)
asl.l #9,%d0
lsl.l #1,(%a0)
mulu.l #3,%d0
muls.l (0x1000).l,%d0
divs.w %d1,%d0
divs.l #3,%d0
rems.l %d1,%d0:%d0
lea %d0,%a0
lea (%a0),%d0
jmp (%a0)+
movem.l %d0-%d7,-(%sp)
movem.l (%sp)+,%d0-%d7
move.l %d0,%usp
move.w %sr,(%a0)
move.w (%a0),%sr
movec %cacr,%d0
movec %d0,%frob
trap #16
stop #0x10000
link %a0,#40000
link.l %a0,#-8
bset #8,(%a0)
bset #3,(0x1000).l
bra.l .+8
bra.s .+2
bra.s .+200
bra.w .+40000
bra .+40000
bsr ext
move.l (4,%a0,%d1.w),%d0
move.l (4,%a0,%d1.l*8),%d0
move.l (200,%a0,%d1.l),%d0
move.l (40000,%a0),%d0
move.l 2b,%d0
REF
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the ColdFire vocabulary"*|*"ColdFire has no"*|*"FPU"*|\
    *"ISA_B"*|*" is not "*|*"does not fit"*|*"out of reach"*|\
    *"the size is"*|*"are .l only"*|*"cannot move between"*|*"not alterable"*|\
    *"the source is"*|*"the count is"*|*"writes a data register"*|\
    *"and/or take no"*|*"not one of its forms"*|*"takes Dn"*|*"the divisor"*|\
    *"Dr:Dq"*|*"control address"*|*"(An) or (d16,An)"*|*"%usp moves"*|\
    *"goes to a data register"*|*"is written from"*|*"writes control"*|\
    *"the form is"*|*"the vector is"*|*"the operand is"*|*"ColdFire's form"*|\
    *"the bit number"*|*"a constant bit number"*|*"32-bit branch"*|\
    *"word-sized index"*|*"index scale"*|*"inline asm cannot"*|*"refers"*|\
    *"cannot be moved to an address register"*|*"is a symbol"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 60 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "the stack pointer clobbered" "clobbers 'sp', the stack pointer" \
    'void f(void){ __asm__ volatile("nop" ::: "sp"); }'
refc "the frame pointer clobbered" "clobbers 'a6', the frame pointer" \
    'void f(void){ __asm__ volatile("nop" ::: "a6"); }'
refc "an unknown clobber" "is not a ColdFire register" \
    'void f(void){ __asm__ volatile("nop" ::: "eax"); }'
refc "a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("jsr g"); }'
refc "a branch to a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("bsr g"); }'
refc "a named label in a template" "are numeric" \
    'void f(void){ __asm__ volatile("x: bra x"); }'
refc "x86's constraint letters" 'is not valid for ColdFire' \
    'int f(int x){ int r; __asm__("move.l %1,%0" : "=d"(r) : "b"(x)); return r; }'
refc "a 64-bit operand" 'is 8 bytes' \
    'long long f(long long x){ __asm__("nop" :: "d"(x)); return x; }'
refc "a register variable in a1" "is not supported for ColdFire asm" \
    'int f(void){ register int r __asm__("a1") = 1; __asm__("nop" :: "a"(r)); return r; }'
refc "a narrow output in an address register" "moves as a long only" \
    'short f(void){ short r; __asm__("nop" : "=a"(r)); return r; }'
refc "an out-of-range immediate" "the vector is" \
    'void f(void){ __asm__ volatile("trap %0" :: "i"(17)); }'
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a short branch to a symbol elsewhere" "carries no relocation" '	bra.s ext'
refs "a 32-bit branch to a symbol elsewhere" "ISA_B" '	bsr.l ext'
refs "a symbol where none relocates" "cannot relocate" '	move.l (ext,%a0),%d0'
refs ".align 3" "a byte count, a power of two" '	.align 3'
refs "a branch out of reach" "out of reach" '	bra 1f
	.space 40000
1:	rts'
echo "$nr statements, eleven inline templates and five files are each refused by name"
