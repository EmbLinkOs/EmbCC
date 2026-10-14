#!/bin/sh
# -fshort-enums: every enum is the smallest integer type its values fit,
# as arm-none-eabi-gcc builds them by default (clang does not). It is an
# ABI choice -- a struct that holds an enum changes layout -- so it was
# refused by name; now it is a layout, and the layout must be clang's
# under the same flag. An ARM object says which it is (Tag_ABI_enum_size
# 1, against 2 for int enums), and ACLE's __ARM_SIZEOF_MINIMAL_ENUM moves
# with it.
#
# EmbCC's own libc and librt pass no enum type across their interface, so
# they are built -fenum-size-neutral (Tag_ABI_enum_size 0, a claim about
# nothing) and link with programs built either way; embld still refuses
# two objects that do disagree -- and no longer misses a disagreement when
# the first object it looked at stated only the float ABI.
set -u
echo "TEST-MARKER short-enums"
. "$(dirname "$0")/../lib.sh"

command -v clang > /dev/null 2>&1 && command -v llvm-objdump > /dev/null 2>&1 &&
command -v llvm-readelf > /dev/null 2>&1 || { echo "SKIP: needs clang and llvm tools"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/short-enums
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/lay.c" << 'EOF'
enum small { S0, S1, S2 };
enum neg { N0 = -5, N1 = 7 };
enum mid { M0 = 300 };
enum smid { SM0 = -300, SM1 = 300 };
enum big { B0 = 70000 };
struct s { char c; enum small a; enum neg b; enum mid m; enum smid n; enum big g; char z; };
int sizes[] = { sizeof(enum small), sizeof(enum neg), sizeof(enum mid), sizeof(enum smid),
                sizeof(enum big), sizeof(struct s),
                __builtin_offsetof(struct s, a), __builtin_offsetof(struct s, m),
                __builtin_offsetof(struct s, g), __builtin_offsetof(struct s, z),
                (enum neg)-5 < 0 };
EOF
data() { llvm-objdump -s -j .data "$1" | tail -n +5 | cut -c1-43; }
for t in thumbv7em-none-eabi thumbv6m-none-eabi riscv32-unknown-elf x86_64-elf; do
    for f in "" -fshort-enums; do
        "$EMBCC" --target=$t $f -c "$out/lay.c" -o "$out/e.o" || fail "$t $f"
        clang --target=$t $f -c "$out/lay.c" -o "$out/c.o" || fail "clang $t $f"
        [ "$(data "$out/e.o")" = "$(data "$out/c.o")" ] || {
            data "$out/e.o"; echo; data "$out/c.o"
            fail "enum sizes and the struct's layout differ from clang's, $t ${f:-default}"; }
    done
done
echo "short-enums: enum sizes, signedness and a struct's layout are clang's, with and without the flag, on four targets"

# the ARM object says which, and the macro moves
tag() { llvm-readelf --arch-specific "$1" | grep -A2 'TagName: ABI_enum_size' | sed -n 's/.*Description: //p'; }
"$EMBCC" --target=thumbv7em-none-eabi -c "$out/lay.c" -o "$out/i.o"
"$EMBCC" --target=thumbv7em-none-eabi -fshort-enums -c "$out/lay.c" -o "$out/s.o"
"$EMBCC" --target=thumbv7em-none-eabi -fenum-size-neutral -c "$out/lay.c" -o "$out/n.o"
clang --target=thumbv7em-none-eabi -fshort-enums -c "$out/lay.c" -o "$out/cs.o"
[ "$(tag "$out/s.o")" = "$(tag "$out/cs.o")" ] || fail "-fshort-enums: Tag_ABI_enum_size '$(tag "$out/s.o")', clang '$(tag "$out/cs.o")'"
tag "$out/i.o" | grep -qi int || fail "int enums: Tag_ABI_enum_size is '$(tag "$out/i.o")'"
[ "$(tag "$out/n.o")" = "Not Permitted" ] || fail "-fenum-size-neutral: Tag_ABI_enum_size is '$(tag "$out/n.o")', not 0"
"$EMBCC" --target=thumbv7em-none-eabi -fshort-enums -dM -E - < /dev/null | grep -q '__ARM_SIZEOF_MINIMAL_ENUM 1' ||
    fail "-fshort-enums leaves __ARM_SIZEOF_MINIMAL_ENUM at 4"
echo "short-enums: Tag_ABI_enum_size is clang's under the flag, int without it, 0 when neutral; __ARM_SIZEOF_MINIMAL_ENUM is 1"

# EmbCC's libc links with both; two objects that disagree still do not
cat > "$out/img.ld" << 'EOF'
ENTRY(main)
MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 256K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS { .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss : { *(.bss*) *(COMMON) } > RAM }
EOF
printf '#include <stdio.h>\nenum c { R, G };\nstruct p { enum c c; char v; };\nint main(void) { struct p p = { G, 3 }; printf("%%d\\n", (int)sizeof p + p.c); return 0; }\n' > "$out/m.c"
for f in "" -fshort-enums; do
    "$EMBCC" --target=thumbv7em-none-eabi $f -Os "-T$out/img.ld" "$out/m.c" -o "$out/m.elf" \
        2> "$out/m.err" || { cat "$out/m.err"; fail "a ${f:-default} program does not link with libc"; }
done
printf 'enum e { E0 }; int g2(enum e x) { return (int)x; }\n' > "$out/g2.c"
clang --target=thumbv7em-none-eabi -c "$out/g2.c" -o "$out/ci.o"
if "$EMBCC" --target=thumbv7em-none-eabi -nostdlib -e sizes "-T$out/img.ld" "$out/s.o" "$out/ci.o" \
        -o "$out/x.elf" 2> "$out/x.err"; then
    fail "objects with short and int enums linked"
fi
grep -q 'size of an enum' "$out/x.err" || { cat "$out/x.err"; fail "refused, but not for the enum size"; }
# ...and found past an object that states only the float ABI: an object
# with no enum attribute at all is not the yardstick
printf 'int f(void) { return 1; }\n' > "$out/f.c"
"$EMBCC" --target=thumbv7em-none-eabi -fenum-size-neutral -c "$out/f.c" -o "$out/f.o"
if "$EMBCC" --target=thumbv7em-none-eabi -nostdlib -e sizes "-T$out/img.ld" "$out/f.o" "$out/s.o" "$out/ci.o" \
        -o "$out/x.elf" 2> "$out/x2.err"; then
    fail "a neutral object first hid the disagreement after it"
fi
echo "short-enums: a program built either way links with libc; short and int enums together are refused, whatever comes first"

# C++ sizes its own enums: refused for it rather than half done
printf 'enum e { A }; int f() { return sizeof(e); }\n' > "$out/x.cc"
if "$EMBCC" --target=thumbv7em-none-eabi -fshort-enums -c "$out/x.cc" -o "$out/x.o" 2> "$out/cc.err"; then
    fail "-fshort-enums was accepted for C++"
fi
grep -q 'supported for C, not C++' "$out/cc.err" || { cat "$out/cc.err"; fail "the C++ refusal does not say why"; }
echo "short-enums: and refused for C++, by name"
