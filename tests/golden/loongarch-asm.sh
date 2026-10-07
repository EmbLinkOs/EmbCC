#!/bin/sh
# LoongArch64 assembly: the vocabulary against llvm-mc, programs that use
# it on the board, -S reassembled, and what is refused.
#
#  1. tools/laasmcheck assembles every entry of src/arch/loongarch/asm.c's
#     vocabulary -- generated from its own tables -- and the words must
#     equal llvm-mc's for the same lines, pseudo-instructions (li.d, the
#     swapped branches) included; a `.+N` target, the file assembler's
#     spelling of a label, must equal the plain number.
#  2. tests/golden/loongarch-asm/main.c (inline asm of every operand kind,
#     a privileged read, a file-scope asm function, a naked function)
#     linked with forms.S (calls both ways, labels, la.pcrel, la.global,
#     %pc_hi20/%pc_lo12, .dword sym) runs on QEMU's virt board at -O0 and
#     -O2 -- forms.S assembled by EmbCC and, for the reference, by clang.
#  3. -S of main.c, asm blocks included, reassembles with llvm-mc to the
#     object's .text bytes and relocations.
#  4. An instruction outside the vocabulary, an operand out of range, an
#     operator on a number, x86's constraint letters and a callee-saved
#     register are each refused by name.
set -u
echo "TEST-MARKER loongarch-asm"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
QEMU=${EMBCC_QEMU_LOONGARCH:-qemu-system-loongarch64}
CLANG=${EMBCC_REF_CLANG_LOONGARCH:-clang}
T=loongarch64-unknown-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
d=tests/golden/loongarch-asm
out=tests/golden/out/loongarch-asm
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/laasmcheck" \
   tools/laasmcheck/laasmcheck.c src/arch/loongarch/asm.c \
   src/arch/loongarch/emit.c src/arch/code.c src/arch/target.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
   src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "laasmcheck did not build"; exit 1; }

# ---- 1. the vocabulary ---------------------------------------------------
if command -v "$MC" >/dev/null 2>&1 &&
   "$MC" -triple=loongarch64 /dev/null -o /dev/null 2>/dev/null
then
    "$out/laasmcheck" > "$out/v.txt" 2> "$out/v.err" || {
        echo "the assembler refused its own vocabulary:"
        head -3 "$out/v.err"; exit 1; }
    n=$(wc -l < "$out/v.txt" | tr -d ' ')
    [ "$n" -ge 350 ] || { echo "the vocabulary is only $n statements"; exit 1; }
    # each statement, then a marker no entry assembles to, so that a
    # pseudo's several words stay together
    awk -F'@' '{ print $1; print "\tdbar 32767" }' "$out/v.txt" > "$out/v.s"
    "$MC" -triple=loongarch64 -show-encoding "$out/v.s" > "$out/v.mc" \
        2> "$out/v.mcerr" || {
        echo "llvm-mc rejected the vocabulary -- an entry claims an"
        echo "instruction that does not exist:"
        head -4 "$out/v.mcerr"; exit 1; }
    sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\4\3\2\1/p' \
        "$out/v.mc" |
        awk '$1 == "38727fff" { print l; l = ""; next }
             { l = (l == "" ? $1 : l " " $1) }' > "$out/v.ref"
    [ "$(wc -l < "$out/v.ref" | tr -d ' ')" = "$n" ] || {
        echo "llvm-mc encoded a different number of statements"; exit 1; }
    paste -d'@' "$out/v.txt" "$out/v.ref" |
        awk -F'@' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
        > "$out/v.diff"
    if [ -s "$out/v.diff" ]; then
        echo "$(wc -l < "$out/v.diff" | tr -d ' ') statements encode differently from llvm-mc:"
        head -10 "$out/v.diff"; exit 1
    fi
    echo "$n asm statements encode as llvm-mc does"
else
    echo "SKIP the encoding half: no llvm-mc with a LoongArch target"
fi
printf 'b .+8\nb 8\nbeq $a0, $a1, .-16\nbeq $a0, $a1, -16\nbnez $t0, .+4096\nbnez $t0, 4096\nbl .-1024\nbl -1024\n' |
    "$out/laasmcheck" --bytes > "$out/dot.txt" 2> "$out/dot.err" || {
    echo "a .+N target was refused:"; cat "$out/dot.err"; exit 1; }
awk -F'@' 'NR % 2 == 1 { a = $2; next } $2 != a { bad = 1 } END { exit bad }' \
    "$out/dot.txt" || {
    echo "a .+N target does not encode as the plain number:"
    cat "$out/dot.txt"; exit 1; }

# ---- 2. on the board ---------------------------------------------------------
if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_LOONGARCH_HARNESS="$PWD/$out"
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c "tests/harness/loongarch/$f.c" \
            -o "$out/$f.o" || { echo "the harness does not compile"; exit 1; }
    done
    want="1 82 77 976 72 42 1 42 43 1080 1 1000 42 42"
    asms="embcc"
    command -v "$CLANG" >/dev/null 2>&1 &&
        "$CLANG" --target=$T -msoft-float -fsyntax-only -x c /dev/null \
            2>/dev/null && asms="embcc clang"
    for as in $asms; do
        if [ $as = clang ]; then
            "$CLANG" --target=$T -msoft-float -c "$d/forms.S" \
                -o "$out/forms-$as.o" || { echo "clang: forms.S"; exit 1; }
        else
            "$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms-$as.o" || {
                echo "EmbCC could not assemble forms.S"; exit 1; }
        fi
        for opt in -O0 -O2; do
            tag=$as$opt
            "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
                echo "main.c $opt does not compile"; exit 1; }
            EMBLD="$EMBLD" sh tests/harness/loongarch/link.sh "$out/$tag.elf" \
                "$out/main$opt.o" "$out/forms-$as.o" > "$out/$tag.lerr" 2>&1 || {
                echo "$tag: embld could not link it:"; head -3 "$out/$tag.lerr"
                exit 1; }
            sh tests/harness/loongarch/run.sh "$out/$tag.elf" > "$out/$tag.txt"
            got=$(sed -n '1p' "$out/$tag.txt" | sed 's/ *$//')
            [ "$got" = "$want" ] || {
                echo "$tag: the program printed '$got', not '$want'"
                cat "$out/$tag.txt" | head -4; exit 1; }
        done
    done
    echo "inline asm, a file-scope block, a naked function and forms.S"
    echo "(assembled by: $asms) run on the board at -O0 and -O2"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 3. -S, reassembled ---------------------------------------------------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$RE" >/dev/null 2>&1; then
    for opt in -O0 -O2; do
        "$EMBCC" --target=$T $opt -S "$d/main.c" -o "$out/s$opt.s" &&
        "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/c$opt.o" &&
        "$MC" -triple=loongarch64 -filetype=obj "$out/s$opt.s" \
            -o "$out/s$opt.o" || { echo "-S $opt did not reassemble"; exit 1; }
        "$OBJCOPY" -O binary --only-section=.text "$out/c$opt.o" "$out/c.bin"
        "$OBJCOPY" -O binary --only-section=.text "$out/s$opt.o" "$out/s.bin"
        cmp -s "$out/c.bin" "$out/s.bin" || {
            echo "-S $opt reassembles to different .text bytes"; exit 1; }
        for o in c s; do
            "$RE" -r "$out/$o$opt.o" | awk '/R_LARCH/ { print $1, $3 }' |
                sort > "$out/$o.rel"
            "$RE" -s "$out/$o$opt.o" | awk '$5 == "GLOBAL" && $7 != "UND" { print $8 }' |
                sort > "$out/$o.sym"
        done
        cmp -s "$out/c.rel" "$out/s.rel" && cmp -s "$out/c.sym" "$out/s.sym" || {
            echo "-S $opt reassembles to different relocations or symbols:"
            diff "$out/c.rel" "$out/s.rel" | head -4
            diff "$out/c.sym" "$out/s.sym" | head -4; exit 1; }
    done
    echo "-S, asm blocks and naked functions included, reassembles to the object"
fi

# ---- 4. the refusals -------------------------------------------------------------
"$out/laasmcheck" --refuse > "$out/ref.txt" <<'REF'
fadd.d $fa0, $fa1, $fa2
vadd.b $vr0, $vr1, $vr2
addi.d $a0, $a1, 2048
andi $a0, $a1, -1
ori $a0, $a1, 4096
slli.w $a0, $a1, 32
ld.w $a0, $a1, -2049
ll.w $a0, $a1, 2
beq $a0, $a1, 131072
beqz $a0, 6
bl 134217728
amswap_db.w $a0, $a0, $a1
csrxchg $a0, $r1, 0
csrrd $a0, 16384
bstrpick.d $a0, $a1, 3, 4
li.w $a0, 0x100000000
pcalau12i $a0, %pc_hi20(4096)
lu12i.w $a0, %le_hi20(x)
add.d a0, a1, a2
REF
nr=0
while IFS='@' read -r stmt msg; do
    nr=$((nr + 1))
    [ "$msg" = ACCEPTED ] && { echo "'$stmt' was accepted"; exit 1; }
    case $msg in
    *"is not in the LoongArch vocabulary"*|*"does not fit"*|*"is not 0.."*|\
    *"is not a multiple of 4"*|*"rd may not also be"*|*"may not be \$r0 or \$r1"*|\
    *"is not within"*|*"takes a symbol here"*|*"is not a register"*|\
    *"for TLS, the extreme"*) ;;
    *) echo "'$stmt' was refused, but not by name: $msg"; exit 1 ;;
    esac
done < "$out/ref.txt"
[ "$nr" -ge 19 ] || { echo "only $nr refusals were checked"; exit 1; }
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "a callee-saved register in a template" "callee-saved register '\$s0'" \
    'void f(void){ __asm__ volatile("move $s0, $zero"); }'
refc "a callee-saved clobber" "clobbers callee-saved register" \
    'void f(void){ __asm__ volatile("nop" ::: "$fp"); }'
refc "a reserved register" "which the psABI reserves" \
    'void f(void){ __asm__ volatile("move $r21, $zero"); }'
refc "x86's constraint letters" 'is not valid for LoongArch' \
    'int f(int x){ int y; __asm__("move %0, %1" : "=a"(y) : "r"(x)); return y; }'
refc "a callee-saved register variable" 'not supported for LoongArch asm' \
    'long f(void){ register long r __asm__("s3") = 1; __asm__("" : "+r"(r)); return r; }'
refc "an instruction outside the vocabulary" 'is not in the LoongArch vocabulary' \
    'void f(void){ __asm__ volatile("fadd.d $fa0, $fa0, $fa0"); }'
refc "an immediate out of range" 'does not fit' \
    'void f(void){ __asm__ volatile("addi.d $t0, $t0, %0" :: "i"(5000)); }'
printf '\tbeq $a0, $a1, nowhere\n' > "$out/u.s"
if "$EMBCC" --target=$T -c "$out/u.s" -o /dev/null 2> "$out/u.err"; then
    echo "a conditional branch to an undefined symbol was accepted"; exit 1
fi
echo "$nr statements and 7 asm statements in C are refused by name"
