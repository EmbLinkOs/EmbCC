#!/bin/sh
# The TriCore target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_TRICORE, ELF32,
# little-endian, e_flags 0x00200000 = TriCore 1.6.1, RELA relocations of
# the types the backend writes), the machine options a TriCore build
# passes -- a core that executes TriCore 1.6.1 is accepted, any other one
# refused -- the constructs the backend does not lower, each refused with
# a message naming it rather than compiled into something else, and the
# images EmbLD will not build.
set -u
echo "TEST-MARKER tricore-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=tricore-none-elf
out=tests/golden/out/tricore-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in tricore-none-elf tricore-elf tricore-unknown-elf tricore; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = tricore-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
# The header's fields from its bytes, so nothing that knows TriCore is
# needed: class and data, e_machine (44) and e_flags.
set -- $(od -A n -t u1 -N 52 "$out/f.o")
[ "$5" = 1 ] && [ "$6" = 1 ] || { echo "not ELF32 little-endian"; exit 1; }
[ "${19}" = 44 ] && [ "${20}" = 0 ] || {
    echo "e_machine is ${19},${20}, not EM_TRICORE (44)"; exit 1; }
[ "${37}" = 0 ] && [ "${38}" = 0 ] && [ "${39}" = 32 ] && [ "${40}" = 0 ] || {
    echo "e_flags is not 0x00200000 (TriCore 1.6.1)"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    printf 'extern int g; extern int h(int); extern char *p(void);\nint f(void) { return g + h(1) + *p(); }\nint *q = &g;\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -S -r "$out/r.o" > "$out/r.txt" || exit 1
    grep -q '\.rela\.text *RELA ' "$out/r.txt" || {
        echo "the relocations are not RELA (.rela.text)"; exit 1; }
    # readelf has no names for TriCore's types: the numbers, against
    # their symbols -- 24REL (3) for the calls, HIADJ (6) and LO (7) for
    # g's address, 32ABS (2) for the pointer in .data
    for w in '03 .* h \+ 0' '03 .* p \+ 0' '06 .* g \+ 0' '07 .* g \+ 0' \
             '02 .* g \+ 0'; do
        grep -E -q "^[0-9a-f]{8} +[0-9a-f]{6}$w" "$out/r.txt" || {
            echo "no relocation of type and symbol '$w':"
            cat "$out/r.txt"; exit 1; }
    done
    echo "an ELF32 little-endian EM_TRICORE object, e_flags 0x00200000, RELA"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=tc27xx -mcpu=tc39xx -mcpu=tc162 -mtc161 -mtc16 -msoft-float \
         -mlittle-endian -march=tc1.6.1; do
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
refopt -mcpu=tc1797 'not a TriCore 1.6 core'
refopt -mtc13 'not a TriCore 1.6 core'
refopt -mcpu=tc1.3.1 'not a TriCore 1.6 core'
refopt -mhard-float '-mhard-float is not supported'
refopt -mfpu=fpv4-sp-d16 'ARM option'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for tricore-none-elf yet'
done
echo "the TriCore 1.6 cores and soft float are accepted, other cores refused"

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
refc "a 1-byte atomic" 'an atomic narrower than four bytes' \
    'char c; int f(void){ return __atomic_fetch_add(&c, 1, 5); }'
refc "a 2-byte compare-and-swap" 'an atomic narrower than four bytes' \
    'short s; int f(void){ short e = 0; return __atomic_compare_exchange_n(&s, &e, 1, 0, 5, 5); }'
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
refc "a naked function" '__attribute__((naked)) is not supported' \
    '__attribute__((naked)) void f(void){}'
refc "a 16-aligned scalar local" 'needs 16-byte alignment and the stack only guarantees 8' \
    'int f(void){ _Alignas(16) int x = 1; return x; }'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ was accepted"; exit 1
fi
grep -q 'C++ is not yet supported for tricore-none-elf' "$out/cxx.err" || {
    echo "C++ was refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
printf 'nop\n' > "$out/a.s"
if "$EMBCC" --target=$T -c "$out/a.s" -o /dev/null 2> "$out/as.err"; then
    echo "a .s file was accepted"; exit 1
fi
grep -q 'no assembly-file support for tricore-none-elf' "$out/as.err" || {
    echo "a .s file was refused, but not by name:"; cat "$out/as.err"; exit 1; }
refc "an instruction in file-scope asm" 'file-scope asm instruction "nop"' '__asm__("nop");'
echo "narrow and 8-byte atomics, the frame and return address,"
echo "__int128, interrupt and naked functions, an over-aligned scalar, C++"
echo "and assembly files are each refused by name"

# ---- the link -------------------------------------------------------------
printf 'void _start(void){ for (;;) ; }\n' > "$out/s.c"
"$EMBCC" --target=$T -c "$out/s.c" -o "$out/s.o" || exit 1
reflink() {         # reflink WHAT PATTERN ARGS...
    what=$1; pat=$2; shift 2
    if "$EMBLD" -e _start -Ttext 0x80000000 "$@" "$out/s.o" -o "$out/s.elf" \
           2> "$out/ld.err"; then
        echo "embld accepted $what"; exit 1
    fi
    grep -q -- "$pat" "$out/ld.err" || {
        echo "embld refused $what, but not by name:"; cat "$out/ld.err"; exit 1; }
}
reflink "-Tstack without --csa" 'needs --csa START:END' -Tstack 0xa1400000
reflink "an unaligned CSA range" 'must be 64-byte aligned' \
    -Tstack 0xa1400000 --csa 0x80180010:0x80200000
reflink "a CSA range a link word cannot name" 'first 4 MiB of one 256 MiB segment' \
    -Tstack 0xa1400000 --csa 0xa1000000:0xa1010000
"$EMBLD" -e _start -Ttext 0x80000000 -Tstack 0xa1400000 \
    --csa 0x80180000:0x80200000 "$out/s.o" -o "$out/s.elf" || {
    echo "embld refused a well-formed TriCore image"; exit 1; }
echo "EmbLD refuses a TriCore stack without context-save areas, and CSAs it"
echo "cannot link"
