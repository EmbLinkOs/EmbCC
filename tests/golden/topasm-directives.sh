#!/bin/sh
# File-scope asm directives do what they say, or are refused.
#
# .data, .rodata, .bss and other sections, .hidden, .weak, .type and the
# alignment directives were all accepted and did nothing: data meant to
# be written went into .text, padding was never inserted, and every
# global label came out a strong STT_FUNC. On Thumb a function's symbol
# had no bit 0, so a call through a pointer to one switched to the ARM
# state a Cortex-M does not have and faulted. `.quad SYMBOL` on a 32-bit
# target carried a 64-bit relocation kind, and a relocation against a
# label defined in the block went out as a second, undefined symbol.
set -u
echo "TEST-MARKER topasm-directives"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/topasm-directives
rm -rf "${out:?}"; mkdir -p "$out"
command -v llvm-readelf > /dev/null 2>&1 || { echo "SKIP: no llvm-readelf"; exit 0; }

# ---- refused, by name ----------------------------------------------------
refuse() {             # refuse TARGET 'asm text' 'expected message'
    printf '__asm__("%s");\n' "$2" > "$out/r.c"
    if "$EMBCC" --target=$1 -c "$out/r.c" -o "$out/r.o" 2> "$out/r.err"; then
        echo "FAIL $1: accepted: $2"; exit 1
    fi
    grep -q "$3" "$out/r.err" || {
        echo "FAIL $1: '$2' refused without saying '$3':"; cat "$out/r.err"; exit 1; }
}
refuse x86_64-elf '.data\nv: .long 1' 'section'
refuse thumbv7m-none-eabi '.section .rodata\nv: .long 1' 'section'
refuse riscv32-unknown-elf '.data\nv: .long 0' 'section'
refuse x86_64-elf '.globl f\n.hidden f\nf: ret' '.hidden'
refuse x86_64-elf '.balign 32\nf: ret' 'at most 16'
refuse thumbv7m-none-eabi 'p: .quad main' '8-byte symbol address'
refuse x86_64-elf 'p: .long main' 'size of an address'
refuse x86_64-elf '.thumb_func\nf: ret' 'Thumb'
refuse x86_64-elf '.globl f\n.type f, %gnu_indirect_function\nf: ret' 'function or object'
echo "topasm-directives: other sections, .hidden, wide alignment and a mis-sized address are refused"

# ---- symbols: binding, type, the Thumb bit, alignment --------------------
cat > "$out/s.c" <<'SRC'
__asm__(".text\n.byte 1\n.balign 8\n.globl tbl\n.type tbl, %object\ntbl:\n"
        " .long helper, local1\nlocal1:\n .byte 1\n.p2align 2\n"
        ".globl helper\n.weak helper\n.type helper, %function\nhelper:\n"
        " .byte 0x70, 0x47\n");
extern const void *tbl[2];
const void *get(void) { return tbl[0]; }
SRC
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/s.c" -o "$out/s.o" || {
    echo "FAIL: s.c did not compile"; exit 1; }
llvm-readelf -s "$out/s.o" > "$out/s.sym"
llvm-readelf -r "$out/s.o" > "$out/s.rel"
tbl=$(awk '$NF == "tbl" && $7 != "UND" { print $2 }' "$out/s.sym")
helper=$(awk '$NF == "helper" { print $2 }' "$out/s.sym")
grep -q 'OBJECT *GLOBAL .* tbl$' "$out/s.sym" &&
grep -q 'FUNC *WEAK .* helper$' "$out/s.sym" || {
    echo "FAIL: binding or type:"; cat "$out/s.sym"; exit 1; }
[ "$(grep -c ' tbl$' "$out/s.sym")" = 1 ] || {
    echo "FAIL: tbl has more than one symbol:"; cat "$out/s.sym"; exit 1; }
[ $((0x$tbl % 8)) = 0 ] || { echo "FAIL: tbl at 0x$tbl is not 8-aligned"; exit 1; }
[ $((0x$helper % 2)) = 1 ] || {
    echo "FAIL: helper at 0x$helper has no Thumb bit"; exit 1; }
grep -q "R_ARM_ABS32 .*\.text + $(printf '%x' $((0x$helper)))" "$out/s.rel" || {
    echo "FAIL: tbl[0] does not hold helper with its Thumb bit:"; cat "$out/s.rel"; exit 1; }
echo "topasm-directives: .weak, .type, .balign and .p2align, and Thumb functions have bit 0"

# ---- a Thumb function written in asm, called through pointers -------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the run: no $QEMU"; exit 0; }
T=thumbv7m-none-eabi
H=tests/harness/thumb
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || exit 1
done
cat > "$out/run.c" <<'SRC'
void puts_(const char *s);
/* movs r0, #7 ; bx lr */
__asm__(".text\n.balign 4\n.globl ret7\n.type ret7, %function\nret7:\n"
        " .byte 0x07, 0x20, 0x70, 0x47\n"
        ".p2align 2\n.globl fptab\n.type fptab, %object\nfptab:\n"
        " .long ret7\n");
extern int (*const fptab[1])(void);
int ret7(void);
int main(void)
{
    int (*volatile fp)(void) = ret7;
    puts_(fp() == 7 ? "pointer ok\n" : "pointer BAD\n");
    puts_(fptab[0]() == 7 ? "table ok\n" : "table BAD\n");
    puts_(ret7() == 7 ? "call ok\n" : "call BAD\n");
    puts_("==END==\n");
    return 0;
}
SRC
for O in -O0 -O2; do
    "$EMBCC" --target=$T $O -c "$out/run.c" -o "$out/run.o" || exit 1
    sh "$H/link.sh" "$out/run.elf" "$out/run.o" || { echo "FAIL: link"; exit 1; }
    sh "$H/run.sh" "$out/run.elf" > "$out/run$O.txt" 2>&1
    [ "$(grep -c ' ok$' "$out/run$O.txt")" = 3 ] && grep -q '==END==' "$out/run$O.txt" || {
        echo "FAIL $O: the asm function did not run through its pointer:"
        sed -n '1,8p' "$out/run$O.txt"; exit 1; }
done
echo "topasm-directives: an asm Thumb function runs through a pointer and a table"
