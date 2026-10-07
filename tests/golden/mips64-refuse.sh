#!/bin/sh
# The MIPS64 target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_MIPS, ELF64, in
# the triple's byte order, e_flags 0x80000001 = MIPS64r2 | noreorder with
# no o32 ABI field, RELA relocations, .MIPS.abiflags saying 64-bit GPRs
# and soft float), the machine options a MIPS64 build passes -- the one
# configuration EmbCC emits is accepted, every other one refused -- and
# the constructs the backend does not lower, each with a message naming
# it rather than code that does something else.
set -u
echo "TEST-MARKER mips64-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=mips64el-none-elf
out=tests/golden/out/mips64-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in mips64el-none-elf mips64el-unknown-elf mips64el-elf mips64el; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = mips64el-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
for t in mips64-none-elf mips64-unknown-elf mips64-elf mips64; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = mips64-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'extern int g; int f(int x) { return x + g; }\n' > "$out/f.c"
if command -v "$RE" >/dev/null 2>&1; then
    for t in mips64el-none-elf mips64-none-elf; do
        "$EMBCC" --target=$t -c "$out/f.c" -o "$out/$t.o" &&
        "$RE" -h -S -r -A "$out/$t.o" > "$out/$t.hdr" || {
            echo "-c failed for $t"; exit 1; }
        case $t in mips64el*) d='little endian' ;; *) d='big endian' ;; esac
        grep -q 'Class:.*ELF64' "$out/$t.hdr" &&
        grep -q "Data:.*$d" "$out/$t.hdr" &&
        grep -q 'Machine:.*MIPS' "$out/$t.hdr" &&
        grep -q 'Flags:.*0x80000001' "$out/$t.hdr" &&
        grep -q '\.rela\.text *RELA ' "$out/$t.hdr" &&
        grep -q 'R_MIPS_HIGHEST/R_MIPS_NONE/R_MIPS_NONE' "$out/$t.hdr" &&
        grep -q 'R_MIPS_LO16/R_MIPS_NONE/R_MIPS_NONE' "$out/$t.hdr" &&
        grep -q 'GPR size: 64' "$out/$t.hdr" &&
        grep -q 'FP ABI: Soft float' "$out/$t.hdr" || {
            echo "the $t object is not ELF64 $d MIPS64r2 n64 RELA soft float:"
            grep -E 'Class|Data|Machine|Flags|rela|R_MIPS|GPR|FP ABI' "$out/$t.hdr"
            exit 1; }
    done
    echo "ELF64 EM_MIPS objects in either byte order, e_flags 0x80000001,"
    echo "RELA in n64's r_info layout, 64-bit GPRs, soft float"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=mips64r2 -march=mips64r2 -march=5kc -mcpu=octeon -msoft-float \
         -mabi=64 -EL -mno-abicalls -G0; do
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
refopt -mcpu=mips64r6 'not a MIPS64 Release 2 core'
refopt -march=mips32r2 'not a MIPS64 Release 2 core'
refopt -mhard-float '-mhard-float is not supported'
refopt -mabi=n32 'n64 ABI'
refopt -mabi=32 'n64 ABI'
refopt -EB 'little-endian'
refopt -mabicalls '-mabicalls is not supported'
refopt -G8 'no data in .sdata'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for mips64el-none-elf yet'
done
"$EMBCC" --target=mips64-none-elf -EB -c "$out/f.c" -o /dev/null || {
    echo "-EB was refused for mips64-none-elf"; exit 1; }
if "$EMBCC" --target=mips64-none-elf -EL -c "$out/f.c" -o /dev/null \
       2> "$out/opt.err"; then
    echo "-EL was accepted for mips64-none-elf"; exit 1
fi
grep -q 'big-endian' "$out/opt.err" || {
    echo "-EL was refused for mips64-none-elf, but not by name:"
    cat "$out/opt.err"; exit 1; }
echo "the MIPS64r2/n64/soft-float/-mno-abicalls/-G0 flags are accepted, others refused"

# ---- the constructs ----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE [TRIPLE]
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=${4:-$T} -c "$out/bad.c" -o /dev/null \
           2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "a 1-byte atomic" 'an atomic narrower than four bytes' \
    'char c; int f(void){ return __atomic_fetch_add(&c, 1, 5); }'
refc "a 2-byte compare-and-swap" 'an atomic narrower than four bytes' \
    'short s; int f(void){ short e = 0; return __atomic_compare_exchange_n(&s, &e, 1, 0, 5, 5); }'
refc "a 16-byte atomic" 'a 16-byte atomic' \
    '__int128 x; __int128 f(void){ return __atomic_fetch_add(&x, 1, 5); }'
refc "a 16-byte atomic load" 'a 16-byte atomic' \
    '__int128 x; __int128 f(void){ return __atomic_load_n(&x, 5); }'
refc "a computed goto" 'a computed goto' \
    'int f(int i){ void *t[2]; t[0] = &&a; t[1] = &&b; goto *t[i]; a: return 1; b: return 2; }'
refc "__builtin_return_address" 'n64 code keeps no frame-pointer chain' \
    'void *f(void){ return __builtin_return_address(0); }'
refc "__builtin_frame_address" 'n64 code keeps no frame-pointer chain' \
    'void *f(void){ return __builtin_frame_address(0); }'
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt)) f(void){}'
refc "a 32-aligned scalar local" 'needs 32-byte alignment and the stack only guarantees 16' \
    'int f(void){ _Alignas(32) int x = 1; return x; }'
refc "a doubleword instruction in inline asm" 'is not in the MIPS vocabulary' \
    'long f(long a){ long r; __asm__("daddu %0, %1, %1" : "=r"(r) : "r"(a)); return r; }'
refc "a packed bit-field over 8 bytes, big-endian" \
    'across 9 bytes is not supported on a big-endian target (mips64-none-elf)' \
    'struct __attribute__((packed)) p { char c : 3; long v : 64; } g; long f(void){ return g.v; }' \
    mips64-none-elf
# gas's `la` makes a 32-bit address; a .s file for MIPS64 is refused it
printf '\t.text\nf:\tla $2, g\n\tjr $ra\n\tnop\n' > "$out/la.s"
if "$EMBCC" --target=$T -c "$out/la.s" -o /dev/null 2> "$out/la.err"; then
    echo "la was accepted at MIPS64"; exit 1
fi
grep -q 'la loads a 32-bit address' "$out/la.err" || {
    echo "la was refused, but not by name:"; cat "$out/la.err"; exit 1; }
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=mips64-none-elf -fno-exceptions -c "$out/c.cc" -o /dev/null \
       2> "$out/cxx.err"; then
    echo "C++ was accepted for mips64-none-elf"; exit 1
fi
grep -q 'C++ is not yet supported for mips64-none-elf' "$out/cxx.err" || {
    echo "C++ was refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
# ...and an __int128 variadic argument is accepted: placed at an even slot,
# as clang's va_arg reads it (docs/internals/mips64-plan.md)
printf '%s\n' 'void v(int, ...); void f(__int128 x){ v(1, x); }' > "$out/v.c"
"$EMBCC" --target=$T -c "$out/v.c" -o /dev/null || {
    echo "a variadic __int128 was refused"; exit 1; }
echo "narrow and 16-byte atomics, computed goto, the frame and return address,"
echo "interrupt functions, an over-aligned scalar, MIPS64 instructions in"
echo "inline asm, a 32-bit la, a wide packed bit-field big-endian and C++"
echo "big-endian are each refused by name"
