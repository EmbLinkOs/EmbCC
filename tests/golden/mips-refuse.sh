#!/bin/sh
# The MIPS32 target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_MIPS, ELF32,
# little-endian, e_flags 0x70001001 = MIPS32r2 | o32 | noreorder, REL
# relocations, .MIPS.abiflags saying soft float), the machine options a
# MIPS build passes -- the one configuration EmbCC emits is accepted, every
# other one refused -- and the constructs the backend does not lower, each
# with a message naming it rather than code that does something else.
set -u
echo "TEST-MARKER mips-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=mipsel-none-elf
out=tests/golden/out/mips-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in mipsel-none-elf mipsel-unknown-elf mipsel-elf mipsel; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = mipsel-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
# ...and big-endian, the same core (docs/internals/big-endian.md)
for t in mips-none-elf mips-unknown-elf mips-elf mips; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = mips-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*little endian' "$out/f.hdr" &&
    grep -q 'Machine:.*MIPS' "$out/f.hdr" &&
    grep -q 'Flags:.*0x70001001' "$out/f.hdr" &&
    grep -q 'MIPS_ABIFLAGS' "$out/f.hdr" || {
        echo "the object header is not ELF32 LE MIPS, e_flags 0x70001001, with .MIPS.abiflags:"
        grep -E 'Class|Data|Machine|Flags|abiflags' "$out/f.hdr"; exit 1; }
    "$RE" -A "$out/f.o" | grep -q 'FP ABI: Soft float' || {
        echo ".MIPS.abiflags does not say soft float"; exit 1; }
    printf 'extern int g; int f(void) { return g; }\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -S "$out/r.o" | grep -q '\.rel\.text *REL ' || {
        echo "the relocations are not REL (.rel.text), as o32 requires"; exit 1; }
    echo "an ELF32 little-endian EM_MIPS object, e_flags 0x70001001, REL, soft float"
    "$EMBCC" --target=mips-none-elf -c "$out/r.c" -o "$out/rb.o" &&
    "$RE" -h -S -A "$out/rb.o" > "$out/rb.hdr" || exit 1
    grep -q 'Data:.*big endian' "$out/rb.hdr" &&
    grep -q 'Flags:.*0x70001001' "$out/rb.hdr" &&
    grep -q '\.rel\.text *REL ' "$out/rb.hdr" &&
    grep -q 'FP ABI: Soft float' "$out/rb.hdr" || {
        echo "the mips-none-elf object is not ELF32 big-endian o32 REL soft float:"
        grep -E 'Data|Flags|rel|FP ABI' "$out/rb.hdr"; exit 1; }
    echo "and mips-none-elf's the same, big-endian"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=mips32r2 -march=mips32r2 -march=m4k -mcpu=24kc -msoft-float \
         -mabi=32 -EL -mno-abicalls -G0; do
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
refopt -mcpu=mips32r6 'not a MIPS32 Release 2 core'
refopt -march=mips32 'not a MIPS32 Release 2 core'
refopt -mhard-float '-mhard-float is not supported'
refopt -mabi=n32 'o32 ABI'
refopt -EB 'little-endian'
refopt -mabicalls '-mabicalls is not supported'
refopt -G8 'no data in .sdata'
refopt -mfpu=fpv4-sp-d16 'ARM option'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for mipsel-none-elf yet'
done
"$EMBCC" --target=$T -fno-asynchronous-unwind-tables -c "$out/f.c" -o /dev/null || {
    echo "-fno-asynchronous-unwind-tables was refused"; exit 1; }
refopt -mbig-endian 'little-endian'
for o in -EB -mbig-endian; do
    "$EMBCC" --target=mips-none-elf $o -c "$out/f.c" -o /dev/null 2> "$out/opt.err" || {
        echo "$o was refused for mips-none-elf:"; cat "$out/opt.err"; exit 1; }
done
for o in -EL -mlittle-endian; do
    if "$EMBCC" --target=mips-none-elf $o -c "$out/f.c" -o /dev/null 2> "$out/opt.err"; then
        echo "$o was accepted for mips-none-elf"; exit 1
    fi
    grep -q 'big-endian' "$out/opt.err" || {
        echo "$o was refused for mips-none-elf, but not by name:"; cat "$out/opt.err"; exit 1; }
done
echo "the MIPS32r2/o32/soft-float/-mno-abicalls/-G0 flags are accepted, others refused"
echo "(and the byte order only as the triple says it)"

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
    'long long x; long long f(void){ return __atomic_fetch_add(&x, 1, 5); }'
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
refc "a 16-aligned scalar local" 'needs 16-byte alignment and the stack only guarantees 8' \
    'int f(void){ _Alignas(16) int x = 1; return x; }'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ was accepted"; exit 1
fi
grep -q 'C++ is not yet supported for mipsel-none-elf' "$out/cxx.err" || {
    echo "C++ was refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
echo "narrow and 8-byte atomics, the frame and return address,"
echo "__int128, interrupt functions, an over-aligned scalar and C++ are each"
echo "refused by name (assembly is mips-gas.sh's and mips-exc.sh's)"
