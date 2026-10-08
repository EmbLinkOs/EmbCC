#!/bin/sh
# PowerPC assembly: the vocabulary against llvm-mc and llvm-objdump, a
# file's layout against clang's integrated assembler, programs that use it
# on the board, the relocations, and what is refused.
#
#  1. tools/ppcasmcheck assembles every entry of src/arch/ppc/asm.c's
#     vocabulary -- generated from its own tables -- and llvm-mc
#     (-triple=powerpc -mcpu=e500) assembles the same statements: the
#     bytes must be equal, statement by statement. The same statements, as
#     a .s file EmbCC assembles (src/as/gas.c), must make the same bytes
#     again, and llvm-objdump must read back from EmbCC's object the
#     mnemonic each entry names -- and, where llvm-objdump prints a general
#     form (bt/bf, bclr, tw, rlwinm), its BO, BI, TO or masks. An SPR
#     named in mfspr/mtspr encodes as llvm-mc's mfNAME/mtNAME for it. A
#     numeric label in a template is the distance it names.
#  2. layout.S, assembled by EmbCC and by clang, linked by embld at the
#     same address with the same C: the images and the symbols must be
#     identical -- .align as an exponent padded with nops, local and
#     external calls and branches, @ha/@h/@l of symbols here and
#     elsewhere, data words.
#  3. tests/golden/ppc-asm/main.c (inline asm of every operand kind,
#     MSR[EE] and the SPRGs, a CTR loop on numeric labels, a record form
#     deciding a branch, a call through CTR, values live across templates
#     that clobber callee-saved registers or name others as bare numbers;
#     a file-scope asm function, naked functions, one calling C) linked
#     with forms.S runs on QEMU's ppce500 at -O0, -O1, -O2 and -Os --
#     forms.S assembled by EmbCC and, for the reference, by clang.
#  4. forms.o and a small file carry the relocations each form names:
#     R_PPC_REL14, REL24, ADDR24, ADDR16_HA, _HI, _LO and ADDR32.
#  5. An instruction outside the vocabulary, an operand out of range, a
#     symbol in a template, a reserved register clobbered, and what EmbLD
#     cannot relocate are each refused by name.
set -u
echo "TEST-MARKER ppc-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_PPC:-qemu-system-ppc}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
OC=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
CLANG=${EMBCC_REF_CLANG_PPC:-clang}
T=powerpc-none-eabi
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/ppc-asm
out=tests/golden/out/ppc-asm
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1
have_llvm=1
for x in "$MC" "$OD" "$OC" "$RE"; do
    command -v "$x" >/dev/null 2>&1 || have_llvm=
done
[ -n "$have_llvm" ] && "$MC" -triple=powerpc -mcpu=e500 /dev/null \
    -o /dev/null 2>/dev/null || have_llvm=
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$T -mcpu=e500 -c -x assembler /dev/null -o /dev/null \
        2>/dev/null || CLANG=

cc -std=c99 -Wall -Wextra -o "$out/ppcasmcheck" \
   tools/ppcasmcheck/ppcasmcheck.c src/arch/ppc/asm.c \
   src/arch/ppc/emit.c src/arch/code.c src/arch/target.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
   src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "ppcasmcheck did not build"; exit 1; }

# ---- 1. the vocabulary ---------------------------------------------------------
"$out/ppcasmcheck" --vocab > "$out/v.txt" 2> "$out/v.err" || {
    echo "the assembler refused its own vocabulary:"; head -3 "$out/v.err"
    exit 1; }
n=$(wc -l < "$out/v.txt" | tr -d ' ')
[ "$n" -ge 1000 ] || { echo "the vocabulary is only $n statements"; exit 1; }
cut -d'|' -f3- "$out/v.txt" | sed 's/^/	/' > "$out/v.s"
# the file assembler makes the statement assembler's bytes
"$EMBCC" --target=$T -c "$out/v.s" -o "$out/v.o" 2> "$out/vs.err" || {
    echo "EmbCC could not assemble its vocabulary as a file:"
    head -3 "$out/vs.err"; exit 1; }
if [ -n "$have_llvm" ]; then
    "$OC" -O binary -j .text "$out/v.o" "$out/vo.bin"
    od -An -v -tx1 "$out/vo.bin" | tr -d ' \n' > "$out/vo.hex"
    cut -d'|' -f2 "$out/v.txt" | tr -d '\n' > "$out/ours.hex"
    cmp -s "$out/vo.hex" "$out/ours.hex" || {
        echo "the vocabulary as a .s file makes other bytes than the"
        echo "statement assembler's"; exit 1; }
    "$MC" -triple=powerpc -mcpu=e500 -filetype=obj "$out/v.s" -o "$out/mc.o" \
        2> "$out/mc.err" || {
        echo "llvm-mc rejected the vocabulary -- an entry claims a form it"
        echo "does not have:"; grep error "$out/mc.err" | head -5; exit 1; }
    "$RE" -r "$out/mc.o" | grep -q R_PPC && {
        echo "llvm-mc relocated a vocabulary statement: it read an operand"
        echo "as a symbol"; "$RE" -r "$out/mc.o" | head; exit 1; }
    "$OC" -O binary -j .text "$out/mc.o" "$out/mc.bin"
    od -An -v -tx1 "$out/mc.bin" | tr -d ' \n' > "$out/mc.hex"
    awk -F'|' -v mc="$(cat "$out/mc.hex")" '{
            k = length($2); g = substr(mc, pos + 1, k); pos += k
            if (g != $2) print "  " $3 ":  ours " $2 ", llvm-mc " g }' \
        "$out/v.txt" > "$out/diff.txt"
    if [ -s "$out/diff.txt" ]; then
        echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n statements encode differently from llvm-mc:"
        head -20 "$out/diff.txt"; exit 1
    fi
    # the mnemonic llvm-objdump reads back from EmbCC's own object
    "$OD" --mcpu=e500 -d --no-show-raw-insn "$out/v.o" |
        grep -E '^ *[0-9a-f]+:' |
        sed 's/^[^:]*:[[:space:]]*//; s/[[:space:]][[:space:]]*/ /g; s/ $//' \
        > "$out/dis.txt"
    awk -F'|' '{ k = split($1, e, "+"); for (i = 1; i <= k; i++) print e[i] "|" $3 }' \
        "$out/v.txt" > "$out/exp.txt"
    [ "$(wc -l < "$out/exp.txt")" = "$(wc -l < "$out/dis.txt")" ] || {
        echo "llvm-objdump read $(wc -l < "$out/dis.txt" | tr -d ' ') instructions, not $(wc -l < "$out/exp.txt" | tr -d ' ')"
        exit 1; }
    paste -d'|' "$out/exp.txt" "$out/dis.txt" | awk -F'|' '{
            e = $1; t = $3; c = substr(t, length(e) + 1, 1)
            if (index(t, e) != 1 || (c != "" && c != "," && c != " "))
                print "  " $2 ":  reads back as " t }' > "$out/mdiff.txt"
    if [ -s "$out/mdiff.txt" ]; then
        echo "$(wc -l < "$out/mdiff.txt" | tr -d ' ') statements disassemble as another instruction:"
        head -20 "$out/mdiff.txt"; exit 1
    fi
    # each SPR name, against llvm-mc's mfNAME/mtNAME for it
    "$out/ppcasmcheck" --pairs > "$out/pairs.txt" || {
        echo "an SPR name was refused"; exit 1; }
    cut -d'|' -f3 "$out/pairs.txt" | sed 's/^/	/' > "$out/pairs.s"
    "$MC" -triple=powerpc -mcpu=e500 -filetype=obj "$out/pairs.s" \
        -o "$out/pairs.o" 2> "$out/pairs.err" || {
        echo "llvm-mc rejected a simplified SPR mnemonic:"
        head -3 "$out/pairs.err"; exit 1; }
    "$OC" -O binary -j .text "$out/pairs.o" "$out/pairs.bin"
    od -An -v -tx1 "$out/pairs.bin" | tr -d ' \n' | fold -w8 > "$out/pairs.mc"
    echo >> "$out/pairs.mc"
    cut -d'|' -f2 "$out/pairs.txt" | paste -d'|' - "$out/pairs.mc" |
        paste -d'|' - "$out/pairs.txt" |
        awk -F'|' '$1 != $2 { print "  " $3 ":  ours " $1 ", llvm-mc " $2 " (" $5 ")" }' \
        > "$out/pdiff.txt"
    if [ -s "$out/pdiff.txt" ]; then
        echo "an SPR's name encodes other than llvm-mc's mnemonic for it:"
        cat "$out/pdiff.txt"; exit 1
    fi
    np=$(wc -l < "$out/pairs.txt" | tr -d ' ')
    [ "$np" -ge 12 ] || { echo "only $np SPR names were checked"; exit 1; }
    echo "all $n PowerPC statements encode as llvm-mc encodes them, and read back as written;"
    echo "$np SPR names encode as llvm-mc's mfNAME/mtNAME"
else
    echo "SKIP the llvm half: llvm-mc/llvm-objdump with PowerPC not found"
fi
# a numeric label in a template is the distance it names
printf '%s\n' '1: addic. 3, 3, -1|bne 1b|blr' \
              'addic. 3, 3, -1|bne .-4|blr' \
              'beq cr7, 2f|nop|nop|2: blr' \
              'beq cr7, .+12|nop|nop|blr' |
    "$out/ppcasmcheck" --bytes > "$out/lab.txt" 2> "$out/lab.err" || {
    echo "a template with labels was refused:"; cat "$out/lab.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/lab.txt" || {
    echo "a numeric label does not encode as its distance:"; cat "$out/lab.txt"
    exit 1; }

# ---- 2. a file's layout, against clang's assembler ------------------------------------
printf 'int ext_data[4] = { 1, 2, 3, 4 };\nvoid ext_fn(void) { }\nvoid ext_tail(void) { }\n' \
    > "$out/stubs.c"
"$EMBCC" --target=$T -O1 -c "$out/stubs.c" -o "$out/stubs.o" &&
"$EMBCC" --target=$T -c "$d/layout.S" -o "$out/layout.o" || {
    echo "layout.S does not assemble"; exit 1; }
if [ -n "$CLANG" ] && [ -n "$have_llvm" ]; then
    "$CLANG" --target=$T -mcpu=e500 -c "$d/layout.S" -o "$out/layout-clang.o" \
        2> "$out/layout-clang.err" || {
        echo "clang: layout.S"; cat "$out/layout-clang.err"; exit 1; }
    for a in layout layout-clang; do
        "$EMBLD" -e lay_a -Ttext 0x00100000 "$out/$a.o" "$out/stubs.o" \
            -o "$out/$a.elf" > "$out/$a.lerr" 2>&1 || {
            echo "embld could not link $a.o:"; head -3 "$out/$a.lerr"; exit 1; }
        "$OC" -O binary "$out/$a.elf" "$out/$a.bin"
        "$RE" -s "$out/$a.elf" |
            awk '$8 ~ /^(lay_|ext_)/ { print $2, $8 }' | sort -k2 > "$out/$a.sym"
    done
    cmp -s "$out/layout.bin" "$out/layout-clang.bin" || {
        echo "layout.S links to a different image from clang's:"
        cmp "$out/layout.bin" "$out/layout-clang.bin" | head -3; exit 1; }
    cmp -s "$out/layout.sym" "$out/layout-clang.sym" &&
        [ "$(wc -l < "$out/layout.sym" | tr -d ' ')" -ge 7 ] || {
        echo "layout.S's symbols are not where clang puts them:"
        diff "$out/layout.sym" "$out/layout-clang.sym" | head; exit 1; }
    echo "layout.S links to the image clang's object links to: alignment, labels, relocations, data"
else
    echo "SKIP the layout half: no clang with PowerPC"
fi

# ---- 3. on the board ----------------------------------------------------------------
"$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms-embcc.o" || {
    echo "EmbCC could not assemble forms.S"; exit 1; }
if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_PPC_HARNESS="$PWD/$out"
    "$EMBCC" --target=$T -O1 -c tests/harness/ppc/boot.c -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/ppc/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    want1="0 77 78 79 55 2 0 1234 55 -32748 32769 48 54 124076833 37 23 87654321"
    want2="42 43 55 110 2 42 -1 1 7 1 11 -4 3 142 1 -5 4 1 42 8 56 42 5 77 77 77 -32314 1 50 3 4658 42 42 6"
    asms=embcc
    if [ -n "$CLANG" ]; then
        "$CLANG" --target=$T -mcpu=e500 -c "$d/forms.S" -o "$out/forms-clang.o" || {
            echo "clang: forms.S"; exit 1; }
        asms="embcc clang"
    fi
    for as in $asms; do
        for opt in -O0 -O1 -O2 -Os; do
            tag=$as$opt
            [ -f "$out/main$opt.o" ] ||
            "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
                echo "main.c $opt does not compile"; exit 1; }
            EMBLD="$EMBLD" sh tests/harness/ppc/link.sh "$out/$tag.elf" \
                "$out/main$opt.o" "$out/forms-$as.o" > "$out/$tag.lerr" 2>&1 || {
                echo "$tag: embld could not link it:"; head -3 "$out/$tag.lerr"
                exit 1; }
            sh tests/harness/ppc/run.sh "$out/$tag.elf" > "$out/$tag.txt"
            got1=$(sed -n '1p' "$out/$tag.txt" | sed 's/ *$//')
            got2=$(sed -n '2p' "$out/$tag.txt" | sed 's/ *$//')
            [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] || {
                echo "$tag: the program printed"
                head -4 "$out/$tag.txt" | sed 's/^/    /'
                echo "  not"; echo "    $want1"; echo "    $want2"; exit 1; }
        done
    done
    echo "inline asm, a file-scope block, naked functions and forms.S"
    echo "(assembled by: $asms) run on the ppce500 at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 4. the relocations -----------------------------------------------------------------
printf '\tbne cr1, ext_lab\n\tbl ext_fn\n\tlis 3, (ext_data+8)@ha\n\tlwz 3, (ext_data+8)@l(3)\n\tlis 4, ext_data@h\n\tori 4, 4, ext_data@l\n\tb ext_tail\n\tba ext_abs\n\t.long ext_data\n' \
    > "$out/rel.s"
"$EMBCC" --target=$T -c "$out/rel.s" -o "$out/rel.o" || {
    echo "rel.s does not assemble"; exit 1; }
if [ -n "$have_llvm" ]; then
    "$RE" -r "$out/rel.o" | awk '/R_PPC/ { print $1, $3, $5, $7 }' > "$out/rel.txt"
    printf '%s\n' '00000000 R_PPC_REL14 ext_lab 0' \
                  '00000004 R_PPC_REL24 ext_fn 0' \
                  '0000000a R_PPC_ADDR16_HA ext_data 8' \
                  '0000000e R_PPC_ADDR16_LO ext_data 8' \
                  '00000012 R_PPC_ADDR16_HI ext_data 0' \
                  '00000016 R_PPC_ADDR16_LO ext_data 0' \
                  '00000018 R_PPC_REL24 ext_tail 0' \
                  '0000001c R_PPC_ADDR24 ext_abs 0' \
                  '00000020 R_PPC_ADDR32 ext_data 0' > "$out/rel.want"
    cmp -s "$out/rel.txt" "$out/rel.want" || {
        echo "the relocations are not the ones each form names:"
        diff "$out/rel.want" "$out/rel.txt"; exit 1; }
    for r in R_PPC_ADDR16_HA R_PPC_ADDR16_LO R_PPC_REL24 R_PPC_ADDR32; do
        "$RE" -r "$out/forms-embcc.o" | grep -q " $r " || {
            echo "forms.o carries no $r"; exit 1; }
    done
    echo "each symbol form carries its relocation: REL14, REL24, ADDR24, ADDR16_HA/HI/LO, ADDR32"
fi

# ---- 5. the refusals ----------------------------------------------------------------
printf '%s\n' 'fadd 1, 2, 3' 'lfd 1, 0(3)' 'vaddubm 1, 2, 3' 'evaddw 3, 4, 5' \
    'ld 3, 0(4)' 'lswi 3, 4, 8' 'mcrxr 3' 'beq+ .+8' 'bne- cr1, .+8' \
    'addi 3, 4, 32768' 'ori 3, 4, -1' 'li 3, 32768' 'lis 3, 65536' \
    'lwz 3, 32768(4)' 'lwzu 3, 4(3)' 'stwu 3, 4(0)' 'lmw 3, 4(5)' \
    'slwi 3, 4, 32' 'rlwinm 3, 4, 32, 0, 31' 'cmpwi cr8, 3, 0' \
    'cmp 0, 1, 3, 4' 'b .+6' 'b .+33554432' 'beq .+32768' 'bdnzctr' \
    'bl foo' 'lis 3, sym@ha' 'lis 3, 5@sdarel' 'mfspr 3, 1024' 'mtpvr 3' \
    'addi 32, 3, 1' 'add 3, 4' 'wrteei 2' 'tw 32, 3, 4' 'isel 3, 4, 5, 32' \
    'addi. 3, 4, 5' 'foo: nop' '1: b 2f' |
    "$out/ppcasmcheck" --refuse > "$out/ref.txt"
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the PowerPC vocabulary"*|*"floating-point"*|*"AltiVec"*|\
    *"SPE"*|*"64-bit"*|*"Book E"*|*"prediction hint"*|*"does not fit"*|\
    *"update form"*|*"among the registers"*|*"out of range"*|\
    *" is not "*|*"doublewords"*|*"not 4-aligned"*|*"out of reach"*|\
    *"inline asm cannot reach one"*|*"relocation operator"*|\
    *"read-only"*|*" takes "*|*"are numeric"*|*"refers to no label"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 38 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "r1 clobbered" "clobbers 'r1', which holds the stack pointer" \
    'void f(void){ __asm__ volatile("nop" ::: "r1"); }'
refc "r2 clobbered" "clobbers '2', which holds an EABI small-data anchor" \
    'void f(void){ __asm__ volatile("nop" ::: "2"); }'
refc "r13 clobbered" "clobbers 'r13'" \
    'void f(void){ __asm__ volatile("nop" ::: "r13"); }'
refc "an unknown clobber" "is not a PowerPC register" \
    'void f(void){ __asm__ volatile("nop" ::: "eax"); }'
refc "a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("bl g"); }'
refc "a named label in a template" "are numeric" \
    'void f(void){ __asm__ volatile("x: b x"); }'
refc "x86's constraint letters" 'is not valid for PowerPC' \
    'int f(int x){ int r; __asm__("mr %0, %1" : "=r"(r) : "a"(x)); return r; }'
refc "a 64-bit operand" 'is 8 bytes' \
    'long long f(long long x){ __asm__("nop" :: "r"(x)); return x; }'
refc "a register variable in r11" "is not supported for PowerPC asm" \
    'int f(void){ register int r __asm__("r11") = 1; __asm__("nop" :: "r"(r)); return r; }'
refc "a modifier" "modifier '%L' is not supported" \
    'int f(int x){ __asm__("nop # %L0" :: "r"(x)); return x; }'
refc "a wide immediate in a template" "does not fit a signed 16-bit field" \
    'int f(void){ int r; __asm__("li %0, %1" : "=r"(r) : "i"(40000)); return r; }'
refc "r31 changed under alloca" "changes r31, the frame base" \
    'void g(char *); void f(int n){ char *p = __builtin_alloca(n); g(p); __asm__ volatile("li 31, 0"); g(p); }'
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a small-data relocation" "not one EmbLD applies" \
    '	lwz 3, foo@sdarel(13)'
refs "an absolute conditional branch to a symbol" "R_PPC_ADDR14" \
    '	beqa foo'
refs "a symbol where none relocates" "cannot relocate" '	li 3, foo'
refs ".align 17" "the exponent must be" '	.align 17'
refs "a misaligned branch target" "not 4-aligned" '	b 1f
	.byte 0
1:	nop'
echo "$nr statements, twelve inline templates and five files are each refused by name"
