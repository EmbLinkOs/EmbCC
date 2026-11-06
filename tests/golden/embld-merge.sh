#!/bin/sh
# Mergeable strings and .eh_frame, from the compiler through the linker.
#
# embcc writes each C string literal to .rodata.str1.N (SHF_MERGE |
# SHF_STRINGS) under a symbol of its own, and embld keeps one copy of a
# string for the whole image -- one that ends another inside it -- and
# one copy of a CIE, as GNU ld does. Checked here:
#
#   1. two units sharing literals link to one copy each, a tail string
#      inside its container, and every pointer still reaching its bytes
#      (read back out of the image, on a 32-bit and a 64-bit target);
#   2. --no-merge-strings puts them back in .rodata;
#   3. -S assembled is -c's object: the same relocations, against the
#      same .LCn symbols;
#   4. with GCC's AArch64 objects (aligned strings with their padding,
#      CIEs repeated per object), embld's .rodata and .eh_frame are GNU
#      ld's byte for byte, and no zero word ends .eh_frame early.
set -u
export LC_ALL=C
echo "TEST-MARKER embld-merge"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/embld-merge
rm -rf "${out:?}"; mkdir -p "$out"
command -v llvm-readelf >/dev/null 2>&1 && command -v llvm-objcopy >/dev/null 2>&1 ||
    { echo "skipped: needs llvm-readelf and llvm-objcopy"; exit 0; }
EMBCC=${EMBCC:-./embcc}
LD=${EMBLD:-./embld}
fails=0
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

cat > "$out/a.c" <<'EOF'
extern const char *const tab[];
const char *const mine[] = { "hello, world\n", "shared", "a\0b" };
int main(void) { return tab[0] == mine[1]; }
void _start(void) { main(); for (;;) ; }
EOF
cat > "$out/b.c" <<'EOF'
const char *const tab[] = { "shared", "world\n", "shared" };
EOF

# The C string at image offset OFF of the loaded bytes (img.bin).
cstr_at() {
    dd if="$out/img.bin" bs=1 skip="$1" count=64 2>/dev/null | tr '\0' '\n' | head -1
}

for t in thumbv7m-none-eabi riscv64-unknown-elf; do
    "$EMBCC" --target=$t -O1 -c "$out/a.c" -o "$out/a-$t.o" &&
    "$EMBCC" --target=$t -O1 -c "$out/b.c" -o "$out/b-$t.o" ||
        { bad "$t: does not compile"; continue; }
    llvm-readelf -SW "$out/b-$t.o" | grep -q '\.rodata\.str1\.[0-9]* .* AMS ' ||
        bad "$t: no .rodata.str1.N (AMS) section in the object"
    llvm-readelf -rW "$out/a-$t.o" | grep -qE '\.LC[0-9]|\.rodata\.str' ||
        bad "$t: no relocation reaches a string in its mergeable section"
    case $t in
        thumb*) base=0x08000000 ;;
        *)      base=0x80000000 ;;
    esac
    "$LD" -e _start -Ttext $base -o "$out/m-$t.elf" "$out/a-$t.o" "$out/b-$t.o" ||
        { bad "$t: does not link"; continue; }
    # the image as loaded, from the base up (one PT_LOAD: no data here)
    llvm-objcopy -O binary "$out/m-$t.elf" "$out/img.bin"
    n=$(tr '\0' '\n' < "$out/img.bin" | grep -c '^shared$')
    [ "$n" = 1 ] || bad "$t: \"shared\" is in the image $n times, not once"
    n=$(tr '\0' '\n' < "$out/img.bin" | grep -c '^world$')
    [ "$n" = 0 ] || bad "$t: \"world\\n\" has a copy of its own, not the tail of \"hello, world\\n\""
    # every pointer in the two tables still reaches its string
    sz=4; case $t in riscv64*) sz=8 ;; esac
    for sym in mine tab; do
        addr=$(llvm-readelf -sW "$out/m-$t.elf" | awk -v s=$sym '$8 == s { print $2 }')
        [ -n "$addr" ] || { bad "$t: no symbol $sym"; continue; }
        at=$(( 0x$addr - base ))
        for k in 0 1 2; do
            p=$(od -An -t x$sz -j $(( at + k * sz )) -N $sz "$out/img.bin" | tr -d ' ')
            s=$(cstr_at $(( 0x$p - base )))
            case $sym$k in
                mine0) want="hello, world" ;;
                mine1|tab0|tab2) want=shared ;;
                mine2) want=a ;;
                tab1)  want=world ;;
            esac
            [ "$s" = "$want" ] || bad "$t: $sym[$k] points at \"$s\", not \"$want\""
        done
    done
    # --no-merge-strings: all of them in .rodata
    "$EMBCC" --target=$t -O1 --no-merge-strings -c "$out/b.c" -o "$out/bn-$t.o"
    llvm-readelf -SW "$out/bn-$t.o" | grep -q 'rodata\.str' &&
        bad "$t: --no-merge-strings still wrote a .rodata.str section"
done

# -S assembled is -c's object, relocations and string symbols alike
for t in aarch64-elf thumbv7m-none-eabi mips-none-elf xtensa-none-elf loongarch64-none-elf; do
    "$EMBCC" --target=$t -O1 -S "$out/a.c" -o "$out/s-$t.s" &&
    "$EMBCC" --target=$t -c "$out/s-$t.s" -o "$out/s-$t.o" &&
    "$EMBCC" --target=$t -O1 -c "$out/a.c" -o "$out/c-$t.o" || { bad "$t: -S or -c failed"; continue; }
    for o in s c; do
        llvm-readelf -rW "$out/$o-$t.o" | awk '/R_/ { print $1, $3, $5, $6, $7 }' | sort > "$out/$o-$t.rel"
        llvm-readelf -sW "$out/$o-$t.o" | awk '$8 ~ /^\.LC/ { print $2, $3, $4, $5, $8 }' | sort > "$out/$o-$t.sym"
    done
    cmp -s "$out/s-$t.rel" "$out/c-$t.rel" || bad "$t: -S assembles to other relocations than -c's"
    cmp -s "$out/s-$t.sym" "$out/c-$t.sym" || bad "$t: -S's .LCn symbols are not -c's"
done

# GCC's objects against GNU ld
if command -v aarch64-elf-gcc >/dev/null 2>&1 && command -v aarch64-elf-ld >/dev/null 2>&1; then
    cat > "$out/g1.c" <<'EOF'
int puts(const char *);
int f2(int);
int f1(int x) { puts("INF"); puts("0123456789abcdef"); puts(""); return f2(x) + 1; }
void _start(void) { f1(3); for (;;) ; }
EOF
    cat > "$out/g2.c" <<'EOF'
int puts(const char *);
int f2(int x) { puts("nan"); puts("0123456789abcdef"); puts("abcdef"); return x * 2; }
int puts(const char *s) { return *s; }
EOF
    for f in g1 g2; do
        aarch64-elf-gcc -O2 -ffreestanding -fasynchronous-unwind-tables -c "$out/$f.c" -o "$out/$f.o" ||
            bad "aarch64-elf-gcc cannot compile $f.c"
    done
    cat > "$out/g.ld" <<'EOF'
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .eh_frame : { KEEP(*(.eh_frame)) }
}
EOF
    "$LD" -T "$out/g.ld" -o "$out/g.elf" "$out/g1.o" "$out/g2.o" ||
        bad "embld cannot link GCC's AArch64 objects"
    aarch64-elf-ld -T "$out/g.ld" -o "$out/gg.elf" "$out/g1.o" "$out/g2.o" 2>/dev/null ||
        bad "aarch64-elf-ld cannot link them"
    for sec in .rodata .eh_frame; do
        llvm-objcopy -O binary -j $sec "$out/g.elf" "$out/e.bin"
        llvm-objcopy -O binary -j $sec "$out/gg.elf" "$out/g.bin"
        cmp -s "$out/e.bin" "$out/g.bin" || bad "$sec is not GNU ld's"
    done
    llvm-readelf --debug-dump=frames "$out/g.elf" 2>/dev/null | awk '
        /ZERO terminator/ { z = 1; next } z && /FDE|CIE/ { bad = 1 } END { exit bad }' ||
        bad ".eh_frame has a zero word before its last FDE"
else
    echo "note: no aarch64-elf-gcc/ld: GCC's objects not checked"
fi

[ $fails = 0 ] || { echo "embld-merge: $fails failure(s)"; exit 1; }
echo "embld-merge: ok"
