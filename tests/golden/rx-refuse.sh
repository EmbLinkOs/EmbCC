#!/bin/sh
# The Renesas RX target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_RX, ELF32,
# little-endian, e_flags E_FLAG_RX_ABI, RELA relocations, the underscore
# GCC's rx-elf puts on every C symbol), the data model GCC's rx-elf
# default gives (32-bit doubles, unsigned char, long-typed size_t, the
# Microsoft bit-field layout), the machine options an RX build passes --
# the one configuration EmbCC emits is accepted, every other refused --
# and the constructs the backend does not lower, each with a message
# naming it rather than code that does something else.
set -u
echo "TEST-MARKER rx-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=rx-none-elf
out=tests/golden/out/rx-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in rx-none-elf rx-elf rx-unknown-elf rx; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = rx-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int g; int f(int x) { return x + g; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S -s -r "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*little endian' "$out/f.hdr" &&
    grep -q 'Machine:.*\(Renesas RX\|0xad\|173\)' "$out/f.hdr" &&
    grep -q 'Flags:.*0x8' "$out/f.hdr" &&
    grep -q '\.rela\.text *RELA ' "$out/f.hdr" || {
        echo "the object header is not ELF32 LE EM_RX, e_flags 0x8, RELA:"
        grep -E 'Class|Data|Machine|Flags|rela' "$out/f.hdr"; exit 1; }
    grep -q ' _f$' "$out/f.hdr" && grep -q ' _g$' "$out/f.hdr" || {
        echo "the C symbols f and g are not _f and _g in the object:"
        grep -E 'FUNC|OBJECT' "$out/f.hdr"; exit 1; }
    echo "an ELF32 little-endian EM_RX object, e_flags 0x8 (RX ABI), RELA, _-prefixed symbols"
fi

# ---- the data model: what GCC's rx-elf -nofpu predefines and lays out --------
cat > "$out/m.c" <<'C'
#include <stddef.h>
struct b1 { char a; int b:4; };
struct b2 { int a:4; char b:4; int c:4; };
struct b3 { char a:3; char b:7; };
struct b4 { long long a:3; int b:5; };
struct b5 { char a; int :0; char b; };
struct b6 { int a:3; int :0; int b:3; };
struct b7 { char a; long long b; };
struct b8 { short a:3; char b; };
_Static_assert(sizeof(double) == 4 && sizeof(long double) == 4, "binary32 double");
_Static_assert(sizeof(long long) == 8 && _Alignof(long long) == 4, "4-aligned long long");
_Static_assert((char)-1 > 0, "unsigned char");
_Static_assert(_Generic((size_t)0, unsigned long: 1, default: 0), "size_t is unsigned long");
_Static_assert(_Generic((ptrdiff_t)0, long: 1, default: 0), "ptrdiff_t is long");
_Static_assert(sizeof(struct b1) == 8 && sizeof(struct b2) == 12 &&
               sizeof(struct b3) == 2 && sizeof(struct b4) == 12 &&
               sizeof(struct b5) == 2 && sizeof(struct b6) == 8 &&
               sizeof(struct b7) == 12 && sizeof(struct b8) == 4 &&
               _Alignof(struct b5) == 1 && _Alignof(struct b1) == 4,
               "the Microsoft bit-field layout, as rx-elf-gcc gives it");
#if !defined(__RX__) || !defined(__RX600__) || !defined(__RX_LITTLE_ENDIAN__) || \
    !defined(__RX_32BIT_DOUBLES__) || !defined(__RX_ABI__) || \
    defined(__RX_FPU_INSNS__) || __FINITE_MATH_ONLY__ != 0
#error "the predefined macros are not rx-elf-gcc -nofpu's"
#endif
int ok;
C
"$EMBCC" --target=$T -Werror -c "$out/m.c" -o "$out/m.o" 2> "$out/m.err" || {
    echo "the data model is not rx-elf-gcc's:"; cat "$out/m.err"; exit 1; }
echo "binary32 double, unsigned char, long-typed size_t and ptrdiff_t, the"
echo "Microsoft bit-field layout and GCC's -nofpu macros"

# ---- the options -----------------------------------------------------------
for o in -mcpu=rx600 -mcpu=rx610 -mcpu=rx200 -mcpu=rx100 -m32bit-doubles \
         -nofpu -mnofpu -mlittle-endian-data -mrx-abi -msmall-data-limit=0 \
         -mno-pid -mint-register=0 -mallow-string-insns -mrelax; do
    "$EMBCC" --target=$T $o -c "$out/f.c" -o /dev/null 2> "$out/opt.err" || {
        echo "$o was refused:"; cat "$out/opt.err"; exit 1; }
done
refopt() {          # refopt OPTION PATTERN
    if "$EMBCC" --target=$T "$1" -c "$out/f.c" -o /dev/null 2> "$out/opt.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/opt.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/opt.err"; exit 1; }
}
refopt -mcpu=rxv2 'not an RXv1 core'
refopt -m64bit-doubles '-m64bit-doubles is not supported'
refopt -fpu '-fpu is not supported'
refopt -mbig-endian-data 'little-endian only'
refopt -mgcc-abi '-mgcc-abi is not supported'
refopt -msmall-data-limit=8 'small-data area'
refopt -mpid '-mpid is not supported'
refopt -mint-register=2 'reserves no'
refopt -mno-allow-string-insns 'smovf'
refopt -mas100-syntax 'AS100'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for rx-none-elf yet'
done
refopt -S '-S is not supported for rx-none-elf'
echo "the RXv1/little-endian/32-bit-double/no-FPU/RX-ABI flags are accepted, others refused"

# ---- the constructs ----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE [FLAGS]
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T ${4:-} -c "$out/bad.c" -o /dev/null \
           2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "an 8-byte atomic read-modify-write" 'an atomic wider than a register' \
    'long long x; long long f(void){ return __atomic_fetch_add(&x, 1, 5); }' -O1
refc "an 8-byte atomic load" 'an atomic access of 8 bytes is not one access' \
    'long long x; long long f(void){ return __atomic_load_n(&x, 5); }'
refc "__builtin_return_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_return_address(0); }'
refc "__builtin_frame_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_frame_address(0); }'
refc "__int128" '__int128 does not exist on this target' \
    '__int128 x;'
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt)) f(void){}'
refc "inline assembly" 'inline assembly is not supported for rx-none-elf' \
    'int f(void){ __asm__("nop"); return 0; }'
refc "a naked function" 'inline assembly is not supported for rx-none-elf' \
    'void __attribute__((naked)) f(void){ __asm__("rts"); }'
refc "file-scope assembly" 'file-scope assembly is not supported for rx-none-elf' \
    '__asm__(".global x\nx: .long 0");'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ was accepted"; exit 1
fi
grep -q 'C++ is not yet supported for rx-none-elf' "$out/cxx.err" || {
    echo "C++ was refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
echo "8-byte atomics, the frame and return address, __int128,"
echo "interrupt functions, an over-aligned scalar, assembly of every kind and"
echo "C++ are each refused by name"
