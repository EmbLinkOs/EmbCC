#!/bin/sh
# SPARC assembly: the vocabulary against llvm-mc and llvm-objdump, a
# file's layout against clang's integrated assembler, programs that use it
# on the board, the relocations, and what is refused.
#
#  1. tools/spasmcheck assembles every entry of src/arch/sparc/asm.c's
#     vocabulary -- generated from its own tables -- and llvm-mc
#     (-triple=sparc -mcpu=leon3) assembles the same statements: the bytes
#     must be equal, statement by statement. The same statements, as a .s
#     file EmbCC assembles (src/as/gas.c), must make the same bytes again,
#     and llvm-objdump must read back from EmbCC's object the mnemonic
#     each entry names (a swapped opcode reads back as another) -- `,a`
#     included. A numeric label in a template is the distance it names.
#  2. layout.S, assembled by EmbCC and by clang, linked by embld at the
#     same address with the same C: the images and the symbols must be
#     identical -- .align as a byte count padded with nops, local and
#     external calls and branches, %hi/%lo and `set` of symbols here and
#     elsewhere, data words.
#  3. tests/golden/sparc-asm/main.c (inline asm of every operand kind,
#     %psr and %y, a numeric-label loop, an annulled slot, a call through
#     a register, values live across clobbering and register-naming
#     templates; a file-scope asm function, naked functions, one calling
#     C) linked with forms.S runs on QEMU's leon3_generic at -O0, -O1, -O2
#     and -Os -- forms.S assembled by EmbCC and, for the reference, by
#     clang.
#  4. forms.o and a small file carry the relocations each form names:
#     R_SPARC_HI22, LO10, WDISP30, WDISP22 and 32.
#  5. An instruction outside the vocabulary, an operand out of range, a
#     symbol in a template, a reserved register clobbered, and what EmbLD
#     cannot relocate are each refused by name.
set -u
echo "TEST-MARKER sparc-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_SPARC:-qemu-system-sparc}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
OC=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
CLANG=${EMBCC_REF_CLANG_SPARC:-clang}
T=sparc-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/sparc-asm
out=tests/golden/out/sparc-asm
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1
have_llvm=1
for x in "$MC" "$OD" "$OC" "$RE"; do
    command -v "$x" >/dev/null 2>&1 || have_llvm=
done
[ -n "$have_llvm" ] && "$MC" -triple=sparc -mcpu=leon3 /dev/null -o /dev/null \
    2>/dev/null || have_llvm=
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$T -mcpu=leon3 -c -x assembler /dev/null -o /dev/null \
        2>/dev/null || CLANG=

cc -std=c99 -Wall -Wextra -o "$out/spasmcheck" \
   tools/spasmcheck/spasmcheck.c src/arch/sparc/asm.c \
   src/arch/sparc/emit.c src/arch/code.c src/arch/target.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
   src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "spasmcheck did not build"; exit 1; }

# ---- 1. the vocabulary ---------------------------------------------------------
"$out/spasmcheck" --vocab > "$out/v.txt" 2> "$out/v.err" || {
    echo "the assembler refused its own vocabulary:"; head -3 "$out/v.err"
    exit 1; }
n=$(wc -l < "$out/v.txt" | tr -d ' ')
[ "$n" -ge 900 ] || { echo "the vocabulary is only $n statements"; exit 1; }
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
    "$MC" -triple=sparc -mcpu=leon3 -filetype=obj "$out/v.s" -o "$out/mc.o" \
        2> "$out/mc.err" || {
        echo "llvm-mc rejected the vocabulary -- an entry claims a form it"
        echo "does not have:"; grep error "$out/mc.err" | head -5; exit 1; }
    "$RE" -r "$out/mc.o" | grep -q R_SPARC && {
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
    "$OD" --mcpu=leon3 -d --no-show-raw-insn "$out/v.o" |
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
    echo "all $n SPARC statements encode as llvm-mc encodes them, and read back as written"
else
    echo "SKIP the llvm half: llvm-mc/llvm-objdump with SPARC not found"
fi
# a numeric label in a template is the distance it names
printf '%s\n' '1: subcc %o0, 1, %o0|bne 1b|nop' \
              'subcc %o0, 1, %o0|bne .-4|nop' \
              'ba,a 2f|nop|set 0x12345678, %o1|2: retl|nop' \
              'ba,a .+16|nop|set 0x12345678, %o1|retl|nop' |
    "$out/spasmcheck" --bytes > "$out/lab.txt" 2> "$out/lab.err" || {
    echo "a template with labels was refused:"; cat "$out/lab.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/lab.txt" || {
    echo "a numeric label does not encode as its distance:"; cat "$out/lab.txt"
    exit 1; }

# ---- 2. a file's layout, against clang's assembler ------------------------------------
cat > "$out/stubs.c" <<'EOF'
int ext_data[4] = { 1, 2, 3, 4 };
void ext_fn(void) { }
void ext_tail(void) { }
EOF
"$EMBCC" --target=$T -O1 -c "$out/stubs.c" -o "$out/stubs.o" &&
"$EMBCC" --target=$T -c "$d/layout.S" -o "$out/layout.o" || {
    echo "layout.S does not assemble"; exit 1; }
if [ -n "$CLANG" ] && [ -n "$have_llvm" ]; then
    "$CLANG" --target=$T -mcpu=leon3 -c "$d/layout.S" -o "$out/layout-clang.o" \
        2> "$out/layout-clang.err" || {
        echo "clang: layout.S"; cat "$out/layout-clang.err"; exit 1; }
    for a in layout layout-clang; do
        "$EMBLD" -e lay_a -Ttext 0x40000000 "$out/$a.o" "$out/stubs.o" \
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
    echo "SKIP the layout half: no clang with SPARC"
fi

# ---- 3. on the board ----------------------------------------------------------------
"$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms-embcc.o" || {
    echo "EmbCC could not assemble forms.S"; exit 1; }
if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_SPARC_HARNESS="$PWD/$out"
    "$EMBCC" --target=$T -O1 -c tests/harness/sparc/boot.c -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/sparc/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    want1="15 77 55 107 3 1234 55 -4076 48 54 124076833 37 23 87654321"
    want2="42 43 55 110 2 9 1 7 1 11 -4 3 142 301221497 15 56 42 5 77 77 77 77 99 42 -32570 4658 581 19 42 42 6"
    asms=embcc
    if [ -n "$CLANG" ]; then
        "$CLANG" --target=$T -mcpu=leon3 -c "$d/forms.S" -o "$out/forms-clang.o" || {
            echo "clang: forms.S"; exit 1; }
        asms="embcc clang"
    fi
    for as in $asms; do
        for opt in -O0 -O1 -O2 -Os; do
            tag=$as$opt
            [ -f "$out/main$opt.o" ] ||
            "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
                echo "main.c $opt does not compile"; exit 1; }
            EMBLD="$EMBLD" sh tests/harness/sparc/link.sh "$out/$tag.elf" \
                "$out/main$opt.o" "$out/forms-$as.o" > "$out/$tag.lerr" 2>&1 || {
                echo "$tag: embld could not link it:"; head -3 "$out/$tag.lerr"
                exit 1; }
            sh tests/harness/sparc/run.sh "$out/$tag.elf" > "$out/$tag.txt"
            got1=$(sed -n '1p' "$out/$tag.txt" | sed 's/ *$//')
            got2=$(sed -n '2p' "$out/$tag.txt" | sed 's/ *$//')
            [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] || {
                echo "$tag: the program printed"
                head -4 "$out/$tag.txt" | sed 's/^/    /'
                echo "  not"; echo "    $want1"; echo "    $want2"; exit 1; }
        done
    done
    echo "inline asm, a file-scope block, naked functions and forms.S"
    echo "(assembled by: $asms) run on the leon3_generic at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 4. the relocations -----------------------------------------------------------------
printf '\tbne ext_lab\n\tnop\n\tcall ext_fn\n\tnop\n\tsethi %%hi(ext_data+8), %%o0\n\tld [%%o0 + %%lo(ext_data+8)], %%o0\n\tset ext_data, %%o1\n\t.word ext_data\n' \
    > "$out/rel.s"
"$EMBCC" --target=$T -c "$out/rel.s" -o "$out/rel.o" || {
    echo "rel.s does not assemble"; exit 1; }
if [ -n "$have_llvm" ]; then
    "$RE" -r "$out/rel.o" | awk '/R_SPARC/ { print $1, $3, $5, $7 }' > "$out/rel.txt"
    cat > "$out/rel.want" <<'EOF'
00000000 R_SPARC_WDISP22 ext_lab 0
00000008 R_SPARC_WDISP30 ext_fn 0
00000010 R_SPARC_HI22 ext_data 8
00000014 R_SPARC_LO10 ext_data 8
00000018 R_SPARC_HI22 ext_data 0
0000001c R_SPARC_LO10 ext_data 0
00000020 R_SPARC_32 ext_data 0
EOF
    cmp -s "$out/rel.txt" "$out/rel.want" || {
        echo "the relocations are not the ones each form names:"
        diff "$out/rel.want" "$out/rel.txt"; exit 1; }
    for r in R_SPARC_HI22 R_SPARC_LO10 R_SPARC_WDISP30 R_SPARC_32; do
        "$RE" -r "$out/forms-embcc.o" | grep -q " $r " || {
            echo "forms.o carries no $r"; exit 1; }
    done
    echo "each symbol form carries its relocation: WDISP22, WDISP30, HI22, LO10, 32"
fi

# ---- 5. the refusals ----------------------------------------------------------------
"$out/spasmcheck" --refuse > "$out/ref.txt" <<'REF'
faddd %f0, %f2, %f4
ldf [%o0], %f1
ldx [%o0], %o1
membar 15
bne,pt .+8
cb1 .+8
add %o0, 4096, %o1
add %o0, -4097, %o1
sll %o0, 32, %o1
ld [%o0 + 4096], %o1
ldd [%o0], %o1
lda [%o0 + 4] 10, %o1
lda [%o0] 256, %o1
ld [%o0] 10, %o1
ba .+6
ba .+8388608
call .+2
bne foo
call foo
or %g1, %lo(sym), %g1
sethi 0x400000, %o0
unimp 0x400000
ta 128
rd %asr32, %o0
wr %o0, %o1, %pc
add %o0, %x1, %o2
mov %o0
casa [%o0 + 4] 10, %o1, %o2
jmpl [%o0], %g0
set 0x100000000, %o0
nop,a
stbar %o0
foo: nop
1: ba 2b
REF
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the SPARC vocabulary"*|*"floating-point"*|*"V9"*|\
    *"coprocessor"*|*"does not fit"*|*"the shift is 0..31"*|*" is odd"*|\
    *"alternate-space"*|*"ASI"*|*"not 4-aligned"*|*"out of reach"*|\
    *"inline asm cannot reach one"*|*" is not "*|*" takes "*|\
    *"one register"*|*"without brackets"*|*"only a branch"*|\
    *"are numeric"*|*"refers to no label"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 34 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "%sp clobbered" "clobbers 'sp', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "sp"); }'
refc "%fp clobbered" "clobbers '%fp', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "%fp"); }'
refc "%i7 clobbered" "clobbers 'i7', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "i7"); }'
refc "an unknown clobber" "is not a SPARC register" \
    'void f(void){ __asm__ volatile("nop" ::: "eax"); }'
refc "a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("call g\n nop"); }'
refc "a named label in a template" "are numeric" \
    'void f(void){ __asm__ volatile("x: ba x\n nop"); }'
refc "x86's constraint letters" 'is not valid for SPARC' \
    'int f(int x){ int r; __asm__("mov %1, %0" : "=r"(r) : "a"(x)); return r; }'
refc "a 64-bit operand" 'is 8 bytes' \
    'long long f(long long x){ __asm__("nop" :: "r"(x)); return x; }'
refc "a register variable in %g1" "is not supported for SPARC asm" \
    'int f(void){ register int r __asm__("g1") = 1; __asm__("nop" :: "r"(r)); return r; }'
refc "a modifier" "modifier '%H' is not supported" \
    'int f(int x){ __asm__("nop ! %H0" :: "r"(x)); return x; }'
refc "a wide immediate in a template" "does not fit the signed 13 bits" \
    'int f(void){ int r; __asm__("mov %1, %0" : "=r"(r) : "i"(5000)); return r; }'
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a V9 relocation operator" "SPARC V9's or position-independent" \
    '	sethi %hh(foo), %o0'
refs "%hi outside sethi" "%hi(symbol) is sethi's" '	or %o0, %hi(foo), %o0'
refs "a symbol where none relocates" "cannot relocate" '	mov foo, %o0'
refs ".align 3" "a power of two" '	.align 3'
refs "a misaligned branch target" "not 4-aligned" '	ba 1f
	nop
	.byte 0
1:	nop'
echo "$nr statements, eleven inline templates and five files are each refused by name"
