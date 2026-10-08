#!/bin/sh
# RX assembly: the vocabulary byte for byte against GNU as and as text
# against GNU objdump, a file's layout against GNU as, programs that use it
# on the board, and what is refused.
#
#  1. tools/rxasmcheck prints every entry of src/arch/rx/asm.c's
#     vocabulary -- generated from its own tables: each ALU operation with
#     registers on both sides of r7, immediates at the ends of every li
#     width, memex sources of every size and displacement field, the
#     addressing modes of mov/movu, every control register and PSW flag,
#     every condition, branches at the ends of their reach with and without
#     a size -- with the text rx-elf-objdump prints for it. The statements
#     are assembled into one .s by EmbCC and by GNU as (rx-elf-as); the
#     bytes must be the same, statement by statement, and objdump's reading
#     of EmbCC's object must be the text written for each, so an opcode or
#     a field swapped for another valid one is caught even where GNU as
#     would not be asked.
#  2. layout.S, assembled by EmbCC and by GNU as, linked by embld at the
#     same address: the images and the symbols must be identical --
#     alignment and its padding, relaxed branches across 40 KB, calls and
#     branches to symbols in another object, data words naming symbols.
#  3. tests/golden/rx-asm/main.c (inline asm of every operand kind, PSW,
#     ISP, USP and INTB through mvfc/mvtc, numeric labels and a loop,
#     values live across templates that clobber callee-saved and unlisted
#     registers, a call from a template; a naked function, a file-scope
#     block) linked with forms.S (C calling assembly and assembly calling
#     C, a tail call, a jump table, relaxed branches, the string, bit and
#     divide instructions) runs on QEMU's gdbsim at -O0, -O1, -O2 and -Os
#     -- forms.S assembled by EmbCC and, for the reference, by GNU.
#  4. Instructions outside the vocabulary, operands out of range, a symbol
#     in a template, the stack pointer clobbered, and a .s file's forms
#     that do not relocate are each refused by name.
set -u
echo "TEST-MARKER rx-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
T=rx-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/rx-asm
out=tests/golden/out/rx-asm
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1
# GNU binutils for RX: EMBCC_RX_BIN, ~/EmbRef/rx-elf/bin, or on PATH
RXBIN=${EMBCC_RX_BIN:-$HOME/EmbRef/rx-elf/bin}
if [ -x "$RXBIN/rx-elf-as" ]; then
    GAS=$RXBIN/rx-elf-as; GOBJDUMP=$RXBIN/rx-elf-objdump
    GOBJCOPY=$RXBIN/rx-elf-objcopy; GCC=$RXBIN/rx-elf-gcc
elif command -v rx-elf-as >/dev/null 2>&1; then
    GAS=rx-elf-as; GOBJDUMP=rx-elf-objdump; GOBJCOPY=rx-elf-objcopy
    GCC=rx-elf-gcc
else
    GAS=
fi
OBJCOPY=${EMBCC_OBJCOPY:-llvm-objcopy}
export EMBCC_OBJCOPY="$OBJCOPY"

cc -std=c99 -Wall -Wextra -o "$out/rxasmcheck" \
   tools/rxasmcheck/rxasmcheck.c src/arch/rx/asm.c src/arch/rx/emit.c \
   src/arch/code.c src/arch/target.c src/driver/util.c src/driver/diag.c \
   src/platform/platform_common.c src/platform/platform_posix.c \
   src/sema/type.c src/sema/ldfloat.c || {
    echo "rxasmcheck did not build"; exit 1; }

# ---- 1. the vocabulary -------------------------------------------------------
"$out/rxasmcheck" --vocab > "$out/v.txt" 2> "$out/v.err" || {
    echo "the assembler refused its own vocabulary:"; head -3 "$out/v.err"
    exit 1; }
n=$(wc -l < "$out/v.txt" | tr -d ' ')
[ "$n" -ge 2500 ] || { echo "the vocabulary is only $n statements"; exit 1; }
{ printf '\t.text\n'; cut -d'|' -f2 "$out/v.txt" | sed 's/^/\t/'; } > "$out/v.s"
"$EMBCC" --target=$T -c "$out/v.s" -o "$out/v.o" 2> "$out/vs.err" || {
    echo "EmbCC's .s assembler refused the vocabulary:"; head -3 "$out/vs.err"
    exit 1; }
if [ -n "$GAS" ]; then
    "$GAS" -muse-conventional-section-names "$out/v.s" -o "$out/vg.o" \
        2> "$out/vg.err" || {
        echo "GNU as rejected the vocabulary -- an entry claims a form it"
        echo "does not have:"; grep -i error "$out/vg.err" | head -5; exit 1; }
    "$GOBJCOPY" -O binary -j .text "$out/v.o" "$out/e.bin"
    "$GOBJCOPY" -O binary -j .text "$out/vg.o" "$out/g.bin"
    perl -e '
      my ($v, $e, $g) = @ARGV;
      open V, $v or die; my @L = <V>; close V;
      local $/; open E, $e or die; my $eb = <E>; open G, $g or die; my $gb = <G>;
      my ($off, $bad) = (0, 0);
      for (@L) { chomp; my ($len, $st) = split /\|/, $_, 3;
        my ($x, $y) = (substr($eb, $off, $len), substr($gb, $off, $len));
        if ($x ne $y && $bad++ < 20) {
          printf "  %s:  ours %s, GNU %s\n", $st, unpack("H*", $x), unpack("H*", $y); }
        $off += $len; }
      if (length($eb) != $off || length($gb) != $off) {
        print "  (ours is ", length($eb), " bytes, GNU ", length($gb),
              ", the vocabulary ", $off, ")\n"; $bad++; }
      exit($bad ? 1 : 0);
    ' "$out/v.txt" "$out/e.bin" "$out/g.bin" || {
        echo "statements encode differently from GNU as"; exit 1; }
    echo "all $n RX asm statements encode as GNU as encodes them"
    # objdump's reading of EmbCC's object, statement by statement, against
    # the text the vocabulary wrote for each: immediates compared as
    # values (objdump prints some in hex), a branch's target as an address
    "$GOBJDUMP" -d "$out/v.o" > "$out/v.dis"
    perl -e '
      my ($v, $d) = @ARGV;
      sub imm { my $t = shift;
        $t =~ s/#(-?)0x([0-9a-f]+)/"#".($1 ? -hex($2) : (hex($2) >= 2**31 ? hex($2) - 2**32 : hex($2)))/ge;
        return $t; }
      sub sz { my $t = shift;     # a mov.w/.b immediate, as the access stores it
        $t =~ s/^mov\.w #(-?\d+)/"mov.w #".((($1 & 0xffff) ^ 0x8000) - 0x8000)/e;
        $t =~ s/^mov\.b #(-?\d+)/"mov.b #".($1 & 0xff)/e;
        return $t; }
      my @I;
      open D, $d or die;
      while (<D>) { chomp;
        next unless /^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$/;
        my ($a, $t) = (hex($1), $2);
        $t =~ s/\s*<[^>]*>//g; $t =~ s/\s+/ /g; $t =~ s/ $//;
        if (my ($m, $h) = $t =~ /^(b\S*) (?:0x)?([0-9a-f]+)$/) {
          if ($m !~ /^b(set|clr|tst|not|m)/) {
            my $x = hex($h); $x -= 2**32 if $x >= 2**31; $t = "$m @" . $x; } }
        $t =~ s/^nop ; //;      # objdump marks mul #1 as the nop it is
        push @I, [$a, sz(imm($t))]; }
      close D;
      open V, $v or die; my ($off, $k, $bad) = (0, 0, 0);
      while (<V>) { chomp; my ($len, $st, $want) = split /\|/, $_, 3;
        my @got;
        while ($k < @I && $I[$k][0] < $off + $len) { push @got, $I[$k][1]; $k++; }
        my $got = join("|", @got);
        my $w = join("|", map { my $x = $_; $x =~ s/\{\+(-?\d+)\}/"@".($off + $1)/ge;
                                $x =~ s/\s+/ /g; sz(imm($x)) } split /\|/, $want);
        if ($got ne $w && $bad++ < 20) { print "  $st:  want \"$w\", objdump \"$got\"\n"; }
        $off += $len; }
      exit($bad ? 1 : 0);
    ' "$out/v.txt" "$out/v.dis" || {
        echo "statements disassemble to something other than what was written"
        exit 1; }
    echo "and GNU objdump reads each back as it was written"
else
    echo "SKIP the GNU half: no rx-elf-as (set EMBCC_RX_BIN)"
fi
# a numeric label in a template is the distance it names, and a branch
# written without a size relaxes as in a file
printf '1: sub #1, r2|bne 1b\nsub #1, r2|bne .-2\nbeq 2f|nop|nop|nop|2: rts\nbeq .+4|nop|nop|nop|rts\nbgt 3f|mov.l #1, r1|3: rts\nbgt .+4|mov.l #1, r1|rts\n' |
    "$out/rxasmcheck" --bytes > "$out/lab.txt" 2> "$out/lab.err" || {
    echo "a template with labels was refused:"; cat "$out/lab.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/lab.txt" || {
    echo "a numeric label does not encode as its distance:"; cat "$out/lab.txt"
    exit 1; }

# ---- 2. a file's layout, against GNU as -------------------------------------------
printf 'int ext_data[4] = { 1, 2, 3, 4 };\nvoid ext_fn(void) { }\nvoid ext_tail(void) { }\n' \
    > "$out/stubs.c"
"$EMBCC" --target=$T -O1 -c "$out/stubs.c" -o "$out/stubs.o" &&
"$EMBCC" --target=$T -c "$d/layout.S" -o "$out/layout.o" || {
    echo "layout.S does not assemble"; exit 1; }
if [ -n "$GAS" ]; then
    "$GCC" -c -Wa,-muse-conventional-section-names "$d/layout.S" \
        -o "$out/layout-gnu.o" 2> "$out/layout-gnu.err" || {
        echo "GNU as: layout.S"; cat "$out/layout-gnu.err"; exit 1; }
    for a in layout layout-gnu; do
        "$EMBLD" -e _lay_a -Ttext 0x01800000 "$out/stubs.o" "$out/$a.o" \
            -o "$out/$a.elf" > "$out/$a.lerr" 2>&1 || {
            echo "embld could not link $a.o:"; head -3 "$out/$a.lerr"; exit 1; }
        "$GOBJCOPY" -O binary "$out/$a.elf" "$out/$a.bin"
        ${READELF:-llvm-readelf} -s "$out/$a.elf" |
            awk '$8 ~ /^(_lay_|lay_|_ext_)/ { print $2, $8 }' | sort -k2 \
            > "$out/$a.sym"
    done
    cmp -s "$out/layout.bin" "$out/layout-gnu.bin" || {
        echo "layout.S links to a different image from GNU as's:"
        cmp "$out/layout.bin" "$out/layout-gnu.bin" | head -3; exit 1; }
    cmp -s "$out/layout.sym" "$out/layout-gnu.sym" &&
        [ "$(wc -l < "$out/layout.sym" | tr -d ' ')" -ge 9 ] || {
        echo "layout.S's symbols are not where GNU as puts them:"
        diff "$out/layout.sym" "$out/layout-gnu.sym" | head; exit 1; }
    echo "layout.S links to the image GNU as's object links to: alignment,"
    echo "relaxed branches, labels, relocations"
fi

# ---- 3. on the board -----------------------------------------------------------
if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_RX_HARNESS="$PWD/$out"
    "$EMBCC" --target=$T -O1 -c tests/harness/rx/boot.c -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/rx/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    want1="42 1 0 1 33488896 19088736 21 104 31 18 9 993 203 1291 10 114 1049 74560 291 35 42"
    want2="14 41 14 1352 1 142 10 30 103 100 33423360 8 8 -13986 7 22"
    asms=embcc
    [ -n "$GAS" ] && asms="embcc gnu"
    for as in $asms; do
        if [ $as = gnu ]; then
            "$GCC" -c -Wa,-muse-conventional-section-names "$d/forms.S" \
                -o "$out/forms-$as.o" || { echo "GNU as: forms.S"; exit 1; }
        else
            "$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms-$as.o" || {
                echo "EmbCC could not assemble forms.S"; exit 1; }
        fi
        for opt in -O0 -O1 -O2 -Os; do
            tag=$as$opt
            [ -f "$out/main$opt.o" ] ||
            "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
                echo "main.c $opt does not compile"; exit 1; }
            EMBLD="$EMBLD" sh tests/harness/rx/link.sh "$out/$tag.elf" \
                "$out/main$opt.o" "$out/forms-$as.o" > "$out/$tag.lerr" 2>&1 || {
                echo "$tag: embld could not link it:"; head -3 "$out/$tag.lerr"
                exit 1; }
            sh tests/harness/rx/run.sh "$out/$tag.bin" > "$out/$tag.txt"
            got1=$(sed -n '1p' "$out/$tag.txt" | sed 's/ *$//')
            got2=$(sed -n '2p' "$out/$tag.txt" | sed 's/ *$//')
            [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] &&
                grep -q '==EXIT 0 ==' "$out/$tag.txt" || {
                echo "$tag: the program printed"
                head -4 "$out/$tag.txt" | sed 's/^/    /'
                echo "  not"; echo "    $want1"; echo "    $want2"; exit 1; }
        done
    done
    echo "inline asm, a file-scope block, a naked function and forms.S"
    echo "(assembled by: $asms) run on the gdbsim at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 4. the refusals ---------------------------------------------------------------
"$out/rxasmcheck" --refuse > "$out/ref.txt" <<'REF'
fadd r1, r2
fsub #1, r2
itof r1, r2
movco r1, [r2]
frob r1
mov.q r1, r2
mov sp, r1
mov.l r16, r1
mov.b #1, r1
mov.w #1, r1
movu.l [r1], r2
movu.b r1
mov.l [r1], [r2]
mov.l 2[r1], r2
mov.l -4[r1], r2
mov.w 131072[r1], r2
movu.b 32768[r1], r2
mov.l #1, 131072[r1]
mov.b #256, [r1]
mov.l [r1].w, r2
add #4294967296, r1
sub #4294967295, r1
add 3[r1].l, r2
adc [r1].b, r2
add r1, r2, r3, r4
add.l r1, r2
emul r1, r15
xor r1, r2, r3
shll #32, r1
rotl #1, r2, r3
bset #8, [r1].b
bset #3, [r1]
bset #32, r1
bmeq #8, [r1].b
sceq.b r1
sceq.l [r1]
pushm r0-r3
pushm r5-r3
popm r1
pushc r1
mvtc r1, pc
mvtc r1, frob
mvfc r1, psw
setpsw x
mvtipl #16
int #256
rtsd #3
rtsd #4, r1-r3
racw #3
bra.s .+2
bra.s .+11
bra.b .+128
bra.w .+32768
bsr.b .+8
bgt.s .+5
bgt.w .+5
bra foo
bra 8
bra .+70000000
REF
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the RX vocabulary"*|*"FPU or RXv2"*|*" is not "*|\
    *"does not fit"*|*"out of reach"*|*"out of range"*|*"too many operands"*|\
    *"takes "*|*"these operands"*|*"the size is"*|*"memory-to-memory"*|\
    *"inline asm cannot"*|*"cannot be written"*|*"cannot be it"*|\
    *"is .l only"*|*"is a byte"*|*"the forms are"*|*"the form here"*|\
    *"goes into a register as mov.l"*|*"multiple of 4"*|*"less than"*|\
    *"is not a form RX has"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 55 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "the stack pointer clobbered" "clobbers 'r0', the stack pointer" \
    'void f(void){ __asm__ volatile("nop" ::: "r0"); }'
refc "an unknown clobber" "is not an RX register" \
    'void f(void){ __asm__ volatile("nop" ::: "eax"); }'
refc "a symbol in a template" "inline asm cannot reach one" \
    'void g(void); void f(void){ __asm__ volatile("bsr _g"); }'
refc "a symbol as an immediate in a template" "inline asm cannot take one" \
    'int x; void f(void){ __asm__ volatile("mov.l #_x, r1" ::: "r1"); }'
refc "a named label in a template" "are numeric" \
    'void f(void){ __asm__ volatile("x: bra x"); }'
refc "x86's constraint letters" 'is not valid for RX' \
    'int f(int x){ int r; __asm__("mov %0, %1" : "=r"(r) : "b"(x)); return r; }'
refc "a 64-bit operand" 'is 8 bytes' \
    'long long f(long long x){ __asm__("nop" :: "r"(x)); return x; }'
refc "a register variable in r5" "is not supported for RX asm" \
    'int f(void){ register int r __asm__("r5") = 1; __asm__("nop" :: "r"(r)); return r; }'
refc "a modifier" "modifier '%x' is not supported" \
    'int f(int x){ __asm__("nop ; %x0" :: "r"(x)); return x; }'
refc "an out-of-range immediate" "does not fit" \
    'void f(void){ __asm__ volatile("int %0" :: "i"(300)); }'
refc "an output in a naked function" "has an output" \
    'int g; void __attribute__((naked)) f(void){ __asm__("mov.l #1, %0" : "=r"(g)); }'
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a .s branch to a symbol elsewhere" "carries no relocation" '	bra.s _f'
refs "a symbol where none relocates" "cannot relocate" '	add #_f, r1'
refs ".align 3" "a byte count, a power of two" '	.align 3'
refs "a .s branch out of reach" "out of reach" '	bra.s 1f
	.space 20
1:	rts'
refs "a sized branch out of reach" "out of reach" '	bra.b 1f
	.space 200
1:	rts'
echo "$nr statements, eleven inline templates and five files are each refused by name"
