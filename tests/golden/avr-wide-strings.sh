#!/bin/sh
# Wide string literals on AVR, where wchar_t is two bytes.
#
# L"" was laid out with four-byte elements whatever wchar_t was, so on AVR
# `const wchar_t *p = L"xyz"` read 'x' and then the high half of it: p[1]
# was 0, with no diagnostic. `wchar_t a[] = L"ab"` was refused, and U""
# was typed with the two-byte unsigned int where char32_t is four bytes.
set -u
echo "TEST-MARKER avr-wide-strings"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
out=tests/golden/out/avr-wide-strings
rm -rf "${out:?}"; mkdir -p "$out"
cat > "$out/w.c" <<'SRC'
typedef __WCHAR_TYPE__ wchar_t;
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
void puts_(const char *s);
static const wchar_t *p = L"xyz";
static wchar_t arr[] = L"ab";
static const char32_t *q = U"\U0001F600z";
static const char16_t *r = u"\U0001F600z";
static int check(void)
{
    if (p[0] != 'x' || p[1] != 'y' || p[2] != 'z' || p[3] != 0) return 1;
    if (sizeof arr != 3 * sizeof(wchar_t) || arr[1] != 'b') return 2;
    if (q[0] != 0x1F600 || q[1] != 'z') return 3;
    if (r[0] != 0xD83D || r[1] != 0xDE00 || r[2] != 'z') return 4;
    if (sizeof(L"ab") != 3 * sizeof(wchar_t)) return 5;
    if (sizeof(U"a") != 8 || sizeof(u"a") != 4) return 6;
    return 0;
}
int main(void)
{
    static char msg[] = "result 0\n==END==\n";
    msg[7] = (char)('0' + check());
    puts_(msg);
    return 0;
}
SRC
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" || {
    echo "the harness does not compile"; exit 1; }
for opt in -O0 -Os; do
    "$EMBCC" --target=avr $opt -c "$out/w.c" -o "$out/w.o" &&
    sh tests/harness/avr/link.sh "$out/w.elf" "$out/w.o" > "$out/ln.log" 2>&1 || {
        echo "$opt: does not build"; head -3 "$out/ln.log"; exit 1; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/w.elf" \
              2>/dev/null | grep result)
    [ "$got" = "result 0" ] || { echo "$opt: $got (0 expected)"; exit 1; }
done
echo "avr-wide-strings: L\"\" has two-byte elements, U\"\" four and u\"\" two, at -O0 and -Os"
