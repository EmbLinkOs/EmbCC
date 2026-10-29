#!/bin/sh
# GCC's __builtin_complex(re, im). tests/exec/builtin-complex.c runs it,
# and C11's CMPLX over it, on every board.
#
#  1. Refused as GCC refuses it: parts of two types, an integer or a
#     complex part, a count other than two.
#  2. Of constants it is a constant: a static object's bytes are its two
#     parts, an infinite imaginary part and a negative zero kept -- in
#     binary64, and in AVR's four-byte double -- and so is CMPLX's from
#     EmbCC's <complex.h>, as C11 asks.
set -u
echo "TEST-MARKER builtin-complex"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/builtin-complex
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=riscv32-unknown-elf -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        fail "$1 was accepted"
    fi
    grep -q -- "$2" "$out/bad.err" || { cat "$out/bad.err"; fail "$1 was refused, but not by name"; }
}
refc "parts of two types" "one real floating type, as in GCC, not double and float" \
    'double _Complex f(double a, float b) { return __builtin_complex(a, b); }'
refc "integer parts" "not int and int" \
    'double _Complex f(int a) { return __builtin_complex(a, a); }'
refc "complex parts" "not double _Complex and double _Complex" \
    'double _Complex f(double _Complex a) { return __builtin_complex(a, a); }'
refc "one part" "takes a real and an imaginary part" \
    'double _Complex f(double a) { return __builtin_complex(a); }'
echo "builtin-complex: two types, integer or complex parts, and one part are refused by name"

cat > "$out/s.c" <<'EOF'
#include <complex.h>
double _Complex d = __builtin_complex(-0.0, __builtin_inf());
double _Complex c = CMPLX(1.5, -2.0);
EOF
# bytes WANT TRIPLE: .data, as hex, is WANT
bytes() {
    "$EMBCC" --target=$2 -Ilib/libc/include -c "$out/s.c" -o "$out/s-$2.o" ||
        fail "$2: does not compile"
    llvm-objcopy -O binary --only-section=.data "$out/s-$2.o" "$out/s-$2.bin" ||
        fail "$2: llvm-objcopy"
    got=$(od -An -tx1 "$out/s-$2.bin" | tr -d ' \n')
    [ "$got" = "$1" ] || fail "$2: .data is $got, not $1"
}
# -0.0, +inf, 1.5, -2.0: binary64 little-endian, then AVR's binary32
bytes 0000000000000080000000000000f07f000000000000f83f00000000000000c0 riscv32-unknown-elf
bytes 000000800000807f0000c03f000000c0 avr
echo "builtin-complex: of constants a constant -- a static object's parts, -0.0 and inf kept, binary64 and AVR's binary32, and CMPLX's"
