#!/bin/sh
# -fshort-wchar: wchar_t is unsigned short on every target, as GCC's and
# clang's flag makes it (UEFI; ARM code built for UTF-16 strings). It was
# an unknown argument.
#
#   - the type: 2 bytes, unsigned, unsigned short to _Generic, in C and in
#     C++; L"" and L'' have 16-bit elements, a code point past U+FFFF a
#     surrogate pair; on x86-64, Cortex-M, RV32, AVR and Xtensa;
#   - the macros move with it and match clang's (__WCHAR_TYPE__, _MAX__,
#     _MIN__, _WIDTH__, _UNSIGNED__, __SIZEOF_WCHAR_T__, and ARM's
#     __ARM_SIZEOF_WCHAR_T), so <wchar.h>'s WCHAR_MAX is 65535;
#   - an ARM object says so in Tag_ABI_PCS_wchar_t (2, not 4);
#   - the last of -fshort-wchar and -fno-short-wchar wins;
#   - on the Cortex-M3 board, a wide string's code units are the UTF-16
#     ones.
set -u
echo "TEST-MARKER short-wchar"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/short-wchar
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/t.c" <<'EOF'
#include <stddef.h>
#include <wchar.h>
_Static_assert(sizeof(wchar_t) == 2, "wchar_t is 2 bytes");
_Static_assert((wchar_t)-1 > 0, "wchar_t is unsigned");
_Static_assert(_Generic((wchar_t)0, unsigned short: 1, default: 0),
               "wchar_t is unsigned short");
_Static_assert(_Generic(L"x"[0], unsigned short: 1, default: 0),
               "an L string's element is wchar_t");
_Static_assert(sizeof(L"ab") == 6, "L\"ab\" is three 2-byte units");
_Static_assert(sizeof(L"\U0001F600") == 6, "a surrogate pair and the NUL");
_Static_assert(WCHAR_MAX == 65535 && WCHAR_MIN == 0, "the limits");
_Static_assert(L'\xffff' == 65535, "L'' holds 16 bits, unsigned");
EOF
cat > "$out/t.cc" <<'EOF'
static_assert(sizeof(wchar_t) == 2, "C++ wchar_t is 2 bytes");
static_assert(wchar_t(-1) > 0, "C++ wchar_t is unsigned");
static_assert(sizeof(L"ab") == 6, "C++ L string");
EOF
for T in x86_64-elf thumbv7m-none-eabi riscv32-unknown-elf avr-none-elf xtensa-none-elf; do
    "$EMBCC" --target=$T -fshort-wchar -c "$out/t.c" -o "$out/t.o" \
        > "$out/t.err" 2>&1 || fail "$T -fshort-wchar, C: $(head -2 "$out/t.err")"
    [ $T = avr-none-elf ] && continue        # no C++ on AVR
    "$EMBCC" --target=$T -fshort-wchar -fno-exceptions -c "$out/t.cc" -o "$out/t.o" \
        > "$out/t.err" 2>&1 || fail "$T -fshort-wchar, C++: $(head -2 "$out/t.err")"
done
# without it, and with -fno-short-wchar last, the target's own wchar_t
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/t.c" -o "$out/t.o" 2>/dev/null &&
    fail "thumbv7m without -fshort-wchar has a 2-byte wchar_t"
"$EMBCC" --target=thumbv7m-none-eabi -fshort-wchar -fno-short-wchar -c "$out/t.c" \
    -o "$out/t.o" 2>/dev/null && fail "-fno-short-wchar after -fshort-wchar did not win"
"$EMBCC" --target=thumbv7m-none-eabi -fno-short-wchar -fshort-wchar -c "$out/t.c" \
    -o "$out/t.o" || fail "-fshort-wchar after -fno-short-wchar did not win"
echo "wchar_t is a 16-bit unsigned short in C and C++ on five targets, and only with the flag"

# ---- the macros, against clang ---------------------------------------------
CLANG=${EMBCC_REF_CLANG:-clang}
if command -v "$CLANG" >/dev/null 2>&1; then
    for T in thumbv7m-none-eabi riscv32-unknown-elf; do
        "$EMBCC" --target=$T -fshort-wchar -E -dM -x c /dev/null |
            grep WCHAR | grep -v __CLANG_ | sort > "$out/m-embcc-$T"
        "$CLANG" --target=$T -fshort-wchar -E -dM -x c /dev/null |
            grep WCHAR | grep -v __CLANG_ | sort > "$out/m-clang-$T"
        [ -s "$out/m-embcc-$T" ] || fail "$T: no wchar macros"
        diff "$out/m-embcc-$T" "$out/m-clang-$T" > "$out/m-$T.diff" ||
            fail "$T: the wchar macros differ from clang's: $(cat "$out/m-$T.diff")"
    done
    echo "the wchar macros match clang's on Cortex-M and RV32"
else
    echo "SKIP: the macro comparison (no $CLANG)"
fi

# ---- the ARM build attribute -------------------------------------------------
T=thumbv7m-none-eabi
printf 'int f(void) { return 1; }\n' > "$out/a.c"
for f in "" -fshort-wchar; do
    "$EMBCC" --target=$T $f -c "$out/a.c" -o "$out/a.o" || fail "a.c $f"
    llvm-readelf -A "$out/a.o" > "$out/attrs$f.txt" 2>&1
done
# llvm-readelf -A: "TagName: ABI_PCS_wchar_t", then "Description: N-byte"
grep -A1 "TagName: ABI_PCS_wchar_t" "$out/attrs.txt" | grep -q "Description: 4-byte" ||
    fail "a default object's Tag_ABI_PCS_wchar_t is not 4: $(grep -A1 wchar "$out/attrs.txt")"
grep -A1 "TagName: ABI_PCS_wchar_t" "$out/attrs-fshort-wchar.txt" | grep -q "Description: 2-byte" ||
    fail "a -fshort-wchar object's Tag_ABI_PCS_wchar_t is not 2: $(grep -A1 wchar "$out/attrs-fshort-wchar.txt")"
echo "an ARM object's Tag_ABI_PCS_wchar_t is 2 with the flag and 4 without"

# ---- on the board ---------------------------------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QEMU" >/dev/null 2>&1; then
    d=$out/m3
    mkdir -p "$d"
    cat > "$d/prog.c" <<'EOF'
#include <stddef.h>
void putn(long v);
void puts_(const char *s);
static const wchar_t s[] = L"hé€\U0001F600";
static volatile int i0 = 0;
int main(void)
{
    /* 68 e9 20ac, then d83d de00, then 0: six 16-bit units */
    puts_("units "); putn((long)(sizeof s / sizeof s[0])); puts_("\n");
    puts_("hex ");
    for (int i = i0; s[i]; i++) { putn((long)s[i]); puts_(" "); }
    puts_("\n");
    return 42;
}
EOF
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    "$EMBCC" --target=$T -O2 -fshort-wchar -c "$d/prog.c" -o "$d/prog.o" || fail "prog.c"
    EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" "$d/prog.o" \
        > "$d/link.log" 2>&1 || fail "the M3 image does not link: $(head -3 "$d/link.log")"
    sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$d/prog.elf" > "$d/console.log" 2>&1
    tr -d '\r' < "$d/console.log" > "$d/console.txt"
    grep -q "^units 6" "$d/console.txt" ||
        fail "the string is not six units: $(head -3 "$d/console.txt")"
    # 0x68 0xe9 0x20ac 0xd83d 0xde00, printed in decimal
    tr -s ' ' < "$d/console.txt" | grep -q "^hex 104 233 8364 55357 56832 *$" ||
        fail "the code units are not UTF-16's: $(grep hex "$d/console.txt")"
    echo "on the Cortex-M3 a wide string's units are UTF-16's, a surrogate pair included"
else
    echo "SKIP: the board half ($QEMU not found)"
fi
echo "ok short-wchar"
